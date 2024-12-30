#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <thread>
#include <fstream>
#include <vector>
#include <string>
#include <atomic>
#include <k4a/k4a.h>
#include <k4arecord/record.h>
#include <k4arecord/playback.h>
#include <opencv2/opencv.hpp>

#pragma comment(lib, "Ws2_32.lib")

#define BROADCAST_PORT 8888   // 与服务器广播端口一致
#define TIMEOUT_IN_MS 1000
#define FRAME_DELAY_US 10000
//CV Mat轉換
#include <stdexcept>

cv::Mat k4a_to_cvmat(k4a_image_t k4a_image) {
    if (k4a_image == nullptr) {
        throw std::runtime_error("k4a_image_t is null");
    }

    // 獲取 k4a_image_t 的數據
    int width = k4a_image_get_width_pixels(k4a_image);
    int height = k4a_image_get_height_pixels(k4a_image);
    int stride = k4a_image_get_stride_bytes(k4a_image);
    uint8_t* buffer = k4a_image_get_buffer(k4a_image);
    k4a_image_format_t format = k4a_image_get_format(k4a_image);

    // 根據格式生成 cv::Mat
    switch (format) {
    case K4A_IMAGE_FORMAT_COLOR_BGRA32:
        return cv::Mat(height, width, CV_8UC4, buffer, stride);
    case K4A_IMAGE_FORMAT_DEPTH16:
        return cv::Mat(height, width, CV_16U, buffer, stride);
    case K4A_IMAGE_FORMAT_IR16:
        return cv::Mat(height, width, CV_16U, buffer, stride);
    default:
        throw std::runtime_error("Unsupported k4a_image_format_t");
    }
}

//CV Mat 傳輸相關設定
template <typename T>
class ThreadSafeQueue {
private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cond_var_;
    std::atomic<bool> stop_flag_; // 新增停止标志

public:
    // Push an item into the queue
    void push(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(item);
        cond_var_.notify_one();
    }

    // Pop an item from the queue with blocking
    T wait_and_pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_var_.wait(lock, [this] { return !queue_.empty() || stop_flag_; });

        if (stop_flag_ && queue_.empty()) {
            throw std::runtime_error("Queue stopped and empty");
        }
        T item = queue_.front();
        queue_.pop();
        return item;
    }

    // Stop the queue and notify all waiting threads
    void stop() {
        stop_flag_ = true;
        cond_var_.notify_all();
    }

    T wait_and_front() {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_var_.wait(lock, [this] { return !queue_.empty(); });
        T item = queue_.front();
        return item;
    }

    // Check if the queue is empty
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    // Check if the queue is stopped
    bool is_stopped() const {
        return stop_flag_;
    }
};
struct FrameData {
    cv::Mat image;
    cv::Mat depth_image;
    uint64_t timestamp;
};

std::atomic<uint64_t> recording_stop_timestamp = 0;
std::atomic<uint64_t> recording_start_timestamp = ULLONG_MAX - FRAME_DELAY_US;
int camera_num = 16;

//同步參數
enum CameraType {
    Master,
    Sub,
    Alone,
};
struct CameraReturnStruct
{
    bool start_up_success = false;
    std::string serial_str;
    k4a_calibration_t calibration;
};

void sendMessage(SOCKET socket, int msgType, const std::vector<char>& data) {
    int netMsgType = htonl(msgType);
    int netDataLength = htonl(static_cast<int>(data.size()));

    // 发送消息类型
    int iResult = send(socket, (char*)&netMsgType, sizeof(int), 0);
    if (iResult == SOCKET_ERROR) {
        std::cerr << "send failed (msgType): " << WSAGetLastError() << std::endl;
        return;
    }

    // 发送数据长度
    iResult = send(socket, (char*)&netDataLength, sizeof(int), 0);
    if (iResult == SOCKET_ERROR) {
        std::cerr << "send failed (dataLength): " << WSAGetLastError() << std::endl;
        return;
    }

    // 发送实际数据
    int totalSent = 0;
    while (totalSent < data.size()) {
        iResult = send(socket, data.data() + totalSent, static_cast<int>(data.size()) - totalSent, 0);
        if (iResult == SOCKET_ERROR) {
            std::cerr << "send failed (data): " << WSAGetLastError() << std::endl;
            return;
        }
        totalSent += iResult;
    }
}

void receiveMessage(SOCKET socket, int& msgType, std::vector<char>& data) {
    int iResult;

    // 接收消息类型
    int netMsgType = 0;
    iResult = recv(socket, (char*)&netMsgType, sizeof(int), 0);
    if (iResult <= 0) {
        throw std::runtime_error("recv failed (msgType)");
    }
    msgType = ntohl(netMsgType);

    // 接收数据长度
    int netDataLength = 0;
    iResult = recv(socket, (char*)&netDataLength, sizeof(int), 0);
    if (iResult <= 0) {
        throw std::runtime_error("recv failed (dataLength)");
    }
    int dataLength = ntohl(netDataLength);

    // 接收实际数据
    data.resize(dataLength);
    int totalReceived = 0;
    while (totalReceived < dataLength) {
        iResult = recv(socket, data.data() + totalReceived, dataLength - totalReceived, 0);
        if (iResult <= 0) {
            throw std::runtime_error("recv failed (data)");
        }
        totalReceived += iResult;
    }
}
int CameraOpen(k4a_device_t& device, std::string& serial_str) {
    uint32_t count = k4a_device_get_installed_count();
    if (count == 0)
    {
        std::cerr << "No k4a devices attached!\n" << std::endl;
        return -1;
    }

    // Open the first plugged in Kinect device
    uint32_t camnum = 0;
    while (K4A_FAILED(k4a_device_open(camnum, &device)))
    {
        camnum++;
        if (camnum >= count)
        {
            std::cerr << "Failed to open k4a device!\n" << std::endl;
            return -1;
        }
    }

    // Get the size of the serial number
    size_t serial_size = 0;
    k4a_device_get_serialnum(device, NULL, &serial_size);

    // Allocate memory for the serial, then acquire it
    char* serial = (char*)(malloc(serial_size));
    k4a_device_get_serialnum(device, serial, &serial_size);
    serial_str.assign(serial);
    std::cout << "Opened device: " << serial_str << "\n";
}

int CameraStartup(k4a_device_t &device, std::string &serial_str, k4a_calibration_t &calibration,std::vector<char> &raw_calibration, k4a_device_configuration_t config, int white_balance, int exposure_time) {
    //uint32_t count = k4a_device_get_installed_count();
    //if (count == 0)
    //{
    //    std::cerr << "No k4a devices attached!\n" << std::endl;
    //    return -1;
    //}

    //// Open the first plugged in Kinect device
    //uint32_t camnum = K4A_DEVICE_DEFAULT + K4A_DEVICE_DEFAULT_OFFSET;
    //while (K4A_FAILED(k4a_device_open(camnum, &device)))
    //{
    //    camnum++;
    //    if (camnum >= count)
    //    {
    //        std::cerr << "Failed to open k4a device!\n" << std::endl;
    //        return -1;
    //    } 
    //}

    if (K4A_FAILED(k4a_device_set_color_control(
        device,
        K4A_COLOR_CONTROL_WHITEBALANCE,
        K4A_COLOR_CONTROL_MODE_MANUAL,
        white_balance))) {
        std::cerr << "Failed to set white balance.\n";
    }

    // 设置曝光时间为 20000us
    if (K4A_FAILED(k4a_device_set_color_control(
        device,
        K4A_COLOR_CONTROL_EXPOSURE_TIME_ABSOLUTE,
        K4A_COLOR_CONTROL_MODE_MANUAL,
        exposure_time))) {
        std::cerr << "Failed to set exposure time.\n";
    }

    //// Get the size of the serial number
    //size_t serial_size = 0;
    //k4a_device_get_serialnum(device, NULL, &serial_size);

    //// Allocate memory for the serial, then acquire it
    //char* serial = (char*)(malloc(serial_size));
    //k4a_device_get_serialnum(device, serial, &serial_size);
    //serial_str.assign(serial);
    //std::cout << "Opened device: " << serial_str << "\n";

    // Start the camera with the given configuration
    if (K4A_FAILED(k4a_device_start_cameras(device, &config)))
    {
        std::cerr << "Failed to start cameras!\n" << std::endl;
        return -1;
    }

    // Get calibration data
    if (K4A_FAILED(k4a_device_get_calibration(device, config.depth_mode, config.color_resolution, &calibration)))
    {
        std::cerr << "Failed to get calibration\n" << std::endl;
        k4a_device_close(device);
        return -1;
    }

    // 获取校准数据大小
    size_t calibration_data_size = 0;
    if (k4a_device_get_raw_calibration(device, nullptr, &calibration_data_size) != K4A_BUFFER_RESULT_TOO_SMALL) {
        std::cerr << "Failed to get calibration data size." << std::endl;
        k4a_device_close(device);
        return -1;
    }

    // 创建缓冲区以存储校准数据
    std::vector<uint8_t> calibration_data(calibration_data_size);

    // 获取校准数据
    if (k4a_device_get_raw_calibration(device, calibration_data.data(), &calibration_data_size) != K4A_RESULT_SUCCEEDED) {
        std::cerr << "Failed to get calibration data." << std::endl;
        k4a_device_close(device);
        return -1;
    }

    raw_calibration.resize(calibration_data.size());

    // 使用 std::copy 复制数据
    std::copy(calibration_data.begin(), calibration_data.end(), raw_calibration.begin());

    return 1;
}

//創建工作目錄
#include <filesystem> 
namespace fs = std::filesystem;
int switch_folder(std::string folderName , CameraReturnStruct CRS) {

    // 創建資料夾
    try {
        if (fs::create_directory(folderName)) {
            std::cout << "成功創建資料夾: " << folderName << std::endl;
        }
        else {
            std::cerr << "資料夾已存在或創建失敗: " << folderName << std::endl;
            //return 1;
        }
    }
    catch (const fs::filesystem_error& e) {
        std::cerr << "文件系統錯誤: " << e.what() << std::endl;
        return 1;
    }

    // 切換工作目錄
    try {
        fs::current_path(folderName);
        std::cout << "切換工作目錄至: " << fs::current_path() << std::endl;
    }
    catch (const fs::filesystem_error& e) {
        std::cerr << "無法切換工作目錄: " << e.what() << std::endl;
        return 1;
    }

    if (fs::create_directory(CRS.serial_str)) {
    }
    else {
        std::cerr << "資料夾已存在或創建失敗: " << CRS.serial_str << std::endl;
    }
    if (fs::create_directory(CRS.serial_str + "/color")) {
    }
    else {
        std::cerr << "資料夾已存在或創建失敗: " << CRS.serial_str << std::endl;
    }
    if (fs::create_directory(CRS.serial_str + "/depth")) {
    }
    else {
        std::cerr << "資料夾已存在或創建失敗: " << CRS.serial_str << std::endl;
    }

    return 0;
}

template <typename T>
T receiveAndSetConfiguration(SOCKET connectSocket, int msgTypeE, const T& defaultConfig) {
    T config = defaultConfig; // 使用默认值初始化配置对象
    std::vector<char> config_data;

    // 接收消息
    int msgType;
    receiveMessage(connectSocket, msgType, config_data);

    // 检查消息类型是否成功
    if (msgType == msgTypeE) {
        // 检查数据大小是否正确
        if (config_data.size() >= sizeof(T)) {
            std::memcpy(&config, config_data.data(), sizeof(T));
            //std::cout << "Set device config successfully: sync_mode " << config.wired_sync_mode << std::endl;
        }
        else {
            std::cerr << "Configuration data size mismatch, using default config.\n";
        }
    }
    else {
        std::cerr << "Receive message failed (msgType: " << msgType << "), using default config.\n";
    }

    return config;
}

int main() {
    while (true)
    {
        k4a_device_t device;
        SOCKET FileSocket;
        SOCKET ConnectSocket;
        try{

        bool Stop = false;
        bool Start = false;
        WSADATA wsaData;
        int iResult;

        // 初始化 Winsock
        iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (iResult != 0) {
            std::cerr << "WSAStartup failed: " << iResult << std::endl;
            return 1;
        }

        std::string serverIP = "140.114.24.234";
        int serverPort = 5555;
        int serverFilePort = 8888;


        if (serverIP.empty() || serverPort == 0 || serverFilePort == 0) {
            std::cerr << "Failed to receive server info." << std::endl;
            WSACleanup();
            return 1;
        }

        std::cout << "Received server info: IP=" << serverIP << ", Port=" << serverPort << ", FilePort=" << serverFilePort << std::endl;

        // 创建套接字
        ConnectSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ConnectSocket == INVALID_SOCKET) {
            std::cerr << "socket failed: " << WSAGetLastError() << std::endl;
            WSACleanup();
            return 1;
        }

        // 设置服务器地址和端口
        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        inet_pton(AF_INET, serverIP.c_str(), &serverAddr.sin_addr.s_addr);
        serverAddr.sin_port = htons(serverPort);

        // 连接到服务器
        iResult = connect(ConnectSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr));
        if (iResult == SOCKET_ERROR) {
            std::cerr << "connect failed: " << WSAGetLastError() << std::endl;
            closesocket(ConnectSocket);
            WSACleanup();
            return 1;
        }

        std::cout << "Connected to server communication port." << std::endl;

        // 创建第二个套接字用于文件传输
        FileSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (FileSocket == INVALID_SOCKET) {
            std::cerr << "File socket failed: " << WSAGetLastError() << std::endl;
            closesocket(ConnectSocket);
            WSACleanup();
            return 1;
        }

        // 设置服务器地址和文件传输端口
        sockaddr_in fileServerAddr;
        fileServerAddr.sin_family = AF_INET;
        inet_pton(AF_INET, serverIP.c_str(), &fileServerAddr.sin_addr.s_addr);
        fileServerAddr.sin_port = htons(serverFilePort);

        // 连接到服务器文件传输端口
        iResult = connect(FileSocket, (SOCKADDR*)&fileServerAddr, sizeof(fileServerAddr));
        if (iResult == SOCKET_ERROR) {
            std::cerr << "connect failed (serverFilePort): " << WSAGetLastError() << std::endl;
            closesocket(FileSocket);
            closesocket(ConnectSocket);
            WSACleanup();
            return 1;
        }

        std::cout << "Connected to server file transfer port." << std::endl;

        try {
            //傳送Client相機serial
            //k4a_device_t device;
            struct CameraReturnStruct CRS;
            CameraOpen(device, CRS.serial_str);
            // 分配 vector 的大小與字串長度一致
            std::vector<char> serial_str_data(CRS.serial_str.size());
            // 使用 std::memcpy 拷貝字串內容
            std::memcpy(serial_str_data.data(), CRS.serial_str.data(), CRS.serial_str.size());
            sendMessage(ConnectSocket, -1, serial_str_data);
            //接收相機參數
            k4a_device_configuration_t config = receiveAndSetConfiguration(ConnectSocket, -2, K4A_DEVICE_CONFIG_INIT_DISABLE_ALL);
            int white_balance = receiveAndSetConfiguration(ConnectSocket, -5, 3500);
            int exposure_time = receiveAndSetConfiguration(ConnectSocket, -6, 33000);
            /*k4a_device_configuration_t config = K4A_DEVICE_CONFIG_INIT_DISABLE_ALL;
            int msgType;
            std::vector<char> config_data;
            receiveMessage(ConnectSocket, msgType, config_data);
            if (msgType == -2)
            {
                std::memcpy(&config, config_data.data(), sizeof(config));
                std::cout << "Set device config: sync_mode " << config.wired_sync_mode << std::endl;
            }
            else
            {
                std::cerr << "Set device config fail: " << std::endl;
            }*/
            //設定相機參數
            std::vector<char> raw_calibration;
            if (CameraStartup(device, CRS.serial_str, CRS.calibration, raw_calibration, config, white_balance, exposure_time) < 0)
            {
                std::cerr << "Failed to Startup camera" << std::endl;
                CRS.start_up_success = false;
            }
            else
            {
                CRS.start_up_success = true;
                // 回報設定成功
                std::vector<char> CRS_data(sizeof(CRS));
                std::memcpy(CRS_data.data(), &CRS, sizeof(CRS));
                sendMessage(ConnectSocket, -3, CRS_data);
                sendMessage(ConnectSocket, -4, raw_calibration);
            }
            /*k4a_device_stop_cameras(device);
            k4a_device_close(device);*/
            // 創建錄製線程
            ThreadSafeQueue<FrameData> queue;
            ThreadSafeQueue<FrameData> queue2;
            ThreadSafeQueue<uint64_t> queuePath;
            std::thread recordThread([&queue, device, &Stop, &Start]() {
                while (!Start) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                while (!Stop) {
                    k4a_capture_t capture = NULL;
                    switch (k4a_device_get_capture(device, &capture, TIMEOUT_IN_MS))
                    {
                    case K4A_WAIT_RESULT_SUCCEEDED:
                        break;
                    case K4A_WAIT_RESULT_TIMEOUT:
                        printf("Timed out waiting for a capture\n");
                        continue;
                    case K4A_WAIT_RESULT_FAILED:
                        printf("Failed to read a capture\n");
                        return 1;
                    }

                    // 处理彩色图像
                    k4a_image_t color_image = k4a_capture_get_color_image(capture);
                    k4a_image_t depth_image = k4a_capture_get_depth_image(capture);
                    if (color_image != NULL && depth_image != NULL)
                    {
                        uint64_t timestamp = k4a_image_get_device_timestamp_usec(color_image);
                        // 将K4A图像转换为OpenCV Mat
                        cv::Mat color_mat = k4a_to_cvmat(color_image);
                        cv::Mat depth_mat = k4a_to_cvmat(depth_image);

                        queue.push({ color_mat.clone(),depth_mat.clone(), timestamp });
                        // 释放彩色图像
                        k4a_image_release(color_image);
                        k4a_image_release(depth_image);
                    }

                    // 释放捕获
                    k4a_capture_release(capture);
                }
                k4a_device_stop_cameras(device);
                k4a_device_close(device);
                });
            // 創建圖像資料發送線程
            std::thread recordSendThread([&queue, &queue2, ConnectSocket, &Stop, &Start]() {
                try {
                    while (!Start) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    while (!Stop) {
                        // 從隊列中取出 FrameData
                        FrameData frameData = queue.wait_and_pop();

                        std::vector<char> frameData_timestamp_data(sizeof(frameData.timestamp));
                        std::memcpy(frameData_timestamp_data.data(), &frameData.timestamp, sizeof(frameData.timestamp));
                        sendMessage(ConnectSocket, 3, frameData_timestamp_data);

                        queue2.push(frameData);

                        /* {
                            std::vector<uchar> buffer;
                            cv::imencode(".png", frameData.image, buffer);
                            std::vector<char> char_vector(buffer.begin(), buffer.end());
                            sendMessage(ConnectSocket, 4, char_vector);
                        }


                        {
                            std::vector<uchar> buffer;
                            cv::imencode(".png", frameData.depth_image, buffer);
                            std::vector<char> char_vector(buffer.begin(), buffer.end());
                            sendMessage(ConnectSocket, 6, char_vector);
                        }*/

                        //std::vector<char> frameData_data(sizeof(frameData));
                        //std::memcpy(frameData_data.data(), &frameData, sizeof(frameData));
                        //sendMessage(ConnectSocket, 3, frameData_data);
                        // 顯示圖片
                        //if (!frameData.image.empty()) {
                        //    cv::imshow("Frame", frameData.image);
                        //    std::cout << "Frame size: " << sizeof(frameData.image) << std::endl;

                        //    // 按下 ESC 鍵退出
                        //    if (cv::waitKey(1) == 27) {
                        //        break;
                        //    }
                        //}
                    }
                }
                catch (const std::runtime_error& e) {
                    std::cerr << "Worker thread stopped: " << e.what() << std::endl;
                }
                });
            // 创建接收和发送线程
            std::thread recvThread([ConnectSocket, &Stop, &Start, &queue, &queue2, &CRS, &queuePath]() {
                while (!Stop) {
                    int msgType;
                    std::vector<char> data;
                    receiveMessage(ConnectSocket, msgType, data);

                    if (msgType == 1) { // 文本消息
                        std::string text(data.begin(), data.end());
                        std::cout << "Received text from server: " << text << std::endl;
                    }
                    else if (msgType == 2) { // 文件数据
                        std::string fileName = "received_file_from_server";
                        std::ofstream outFile(fileName, std::ios::binary);
                        if (outFile) {
                            outFile.write(data.data(), data.size());
                            outFile.close();
                            std::cout << "Received file from server saved as " << fileName << std::endl;
                        }
                        else {
                            std::cerr << "Failed to save file from server" << std::endl;
                        }
                    }
                    else if (msgType == 5) { // Camera cmd
                        std::string cmd(data.begin(), data.end());
                        if (cmd == "stop")
                        {
                            std::cout << "Stop Camera " << std::endl;
                            Stop = true;
                            queue.stop();
                            queuePath.stop();
                        }
                        else if (cmd == "start")
                        {
                            std::cout << "Start Camera " << std::endl;
                            Start = true;
                        }
                    }
                    else if (msgType == 7) { // Update delete time stamp
                        FrameData frameDatat;
                        std::memcpy(&frameDatat.timestamp, data.data(), sizeof(frameDatat.timestamp));
                        while (queue2.wait_and_front().timestamp <= frameDatat.timestamp)
                        {
                            FrameData frameData = queue2.wait_and_pop();
                            if (frameData.timestamp < recording_stop_timestamp + FRAME_DELAY_US && frameData.timestamp > recording_start_timestamp + FRAME_DELAY_US)
                            {
                                cv::imwrite(CRS.serial_str + "/color/" + std::to_string(frameData.timestamp) + ".png", frameData.image);
                                cv::imwrite(CRS.serial_str + "/depth/" + std::to_string(frameData.timestamp) + ".png", frameData.depth_image);
                                queuePath.push(frameData.timestamp);
                            }
                        }
                    }
                    else if (msgType == 8) { // Update start time stamp
                        std::memcpy(&recording_start_timestamp, data.data(), sizeof(recording_start_timestamp));
                        std::cout << "Update start time stamp" << std::endl;
                    }
                    else if (msgType == 9) { // Update stop time stamp
                        std::memcpy(&recording_stop_timestamp, data.data(), sizeof(recording_stop_timestamp));
                        std::cout << "Update stop time stamp" << std::endl;
                    }
                    else if (msgType == 10) { // Switch Floder
                        std::string floderName(data.begin(), data.end());
                        switch_folder(floderName, CRS);
                    }
                    else if (msgType == 11) { // Switch Floder
                        std::memcpy(&camera_num, data.data(), sizeof(camera_num));
                        std::cout << "Update camera_num" << std::endl;
                    }
                    else {
                        std::cerr << "Unknown message type from server: " << msgType << std::endl;
                    }
                }
                });

            std::thread sendFileThread([FileSocket, &Stop, &CRS, &queuePath]() {

                try {
                    while (!Stop) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100 * camera_num));
                        FrameData frameData;
                        frameData.timestamp = queuePath.wait_and_pop();
                        frameData.image = cv::imread(CRS.serial_str + "/color/" + std::to_string(frameData.timestamp) + ".png", cv::IMREAD_UNCHANGED);
                        frameData.depth_image = cv::imread(CRS.serial_str + "/depth/" + std::to_string(frameData.timestamp) + ".png", cv::IMREAD_UNCHANGED);
                        std::vector<char> frameData_timestamp_data(sizeof(frameData.timestamp));
                        std::memcpy(frameData_timestamp_data.data(), &frameData.timestamp, sizeof(frameData.timestamp));
                        sendMessage(FileSocket, 1, frameData_timestamp_data);
                        {
                            std::vector<uchar> buffer;
                            cv::imencode(".png", frameData.image, buffer);
                            std::vector<char> char_vector(buffer.begin(), buffer.end());
                            sendMessage(FileSocket, 2, char_vector);
                        }
                        fs::remove(CRS.serial_str + "/color/" + std::to_string(frameData.timestamp) + ".png");
                        {
                            std::vector<uchar> buffer;
                            cv::imencode(".png", frameData.depth_image, buffer);
                            std::vector<char> char_vector(buffer.begin(), buffer.end());
                            sendMessage(FileSocket, 3, char_vector);
                        }
                        fs::remove(CRS.serial_str + "/depth/" + std::to_string(frameData.timestamp) + ".png");
                    }
                }
                catch (const std::runtime_error& e) {
                    std::cerr << "Worker thread stopped: " << e.what() << std::endl;
                }
                });

            recvThread.join();
            sendFileThread.join();
            recordSendThread.join();
            recordThread.join();
        }
        catch (const std::exception& e) {
            std::cerr << "Disconnected from server: " << e.what() << std::endl;
        }

        // 关闭套接字
        closesocket(ConnectSocket);
        closesocket(FileSocket);
        WSACleanup();
        }
        catch (const std::runtime_error& e) {
            std::cerr << "Main thread stopped: " << e.what() << std::endl;
        }
        k4a_device_close(device);
        closesocket(ConnectSocket);
        closesocket(FileSocket);
        WSACleanup();
    }
    return 0;
}
