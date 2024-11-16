﻿#include <winsock2.h>
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
#define TIMEOUT_IN_MS 100

//CV Mat 傳輸相關設定
template <typename T>
class ThreadSafeQueue {
private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cond_var_;

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
        cond_var_.wait(lock, [this] { return !queue_.empty(); });
        T item = queue_.front();
        queue_.pop();
        return item;
    }

    // Check if the queue is empty
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }
};
struct ImagePacket {
    k4a_image_format_t format;
    int width;
    int height;
    int stride;
    uint64_t timestamp;        // 時間戳（微秒）
    std::vector<uint8_t> data; // 圖像數據

    // 將 k4a_image_t 封裝到 ImagePacket
    static ImagePacket from_k4a_image(k4a_image_t image) {
        ImagePacket packet;
        packet.format = k4a_image_get_format(image);
        packet.width = k4a_image_get_width_pixels(image);
        packet.height = k4a_image_get_height_pixels(image);
        packet.stride = k4a_image_get_stride_bytes(image);
        packet.timestamp = k4a_image_get_device_timestamp_usec(image); // 獲取時間戳

        uint8_t* buffer = k4a_image_get_buffer(image);
        size_t size = k4a_image_get_size(image);
        packet.data.assign(buffer, buffer + size); // 將數據拷貝到 vector
        return packet;
    }

    // 從 ImagePacket 解封裝回 k4a_image_t
    k4a_image_t to_k4a_image() const {
        k4a_image_t image;
        if (k4a_image_create(format, width, height, stride, &image) != K4A_RESULT_SUCCEEDED) {
            throw std::runtime_error("無法創建 k4a_image_t");
        }

        uint8_t* dest_buffer = k4a_image_get_buffer(image);
        std::memcpy(dest_buffer, data.data(), data.size()); // 拷貝數據到目標圖像

        // 如果需要，可以在這裡設定時間戳（Azure Kinect SDK 不支持直接設置時間戳）
        return image;
    }

    // 序列化為 std::vector<char>
    std::vector<char> serialize_to_char() const {
        size_t header_size = sizeof(format) + sizeof(width) + sizeof(height) + sizeof(stride) + sizeof(timestamp);
        std::vector<char> buffer(header_size + data.size());

        // 拷貝頭部信息
        size_t offset = 0;
        std::memcpy(buffer.data() + offset, &format, sizeof(format));
        offset += sizeof(format);
        std::memcpy(buffer.data() + offset, &width, sizeof(width));
        offset += sizeof(width);
        std::memcpy(buffer.data() + offset, &height, sizeof(height));
        offset += sizeof(height);
        std::memcpy(buffer.data() + offset, &stride, sizeof(stride));
        offset += sizeof(stride);
        std::memcpy(buffer.data() + offset, &timestamp, sizeof(timestamp));
        offset += sizeof(timestamp);

        // 拷貝圖像數據
        std::memcpy(buffer.data() + offset, data.data(), data.size());

        return buffer;
    }

    // 從 std::vector<char> 反序列化
    static ImagePacket deserialize_from_char(const std::vector<char>& buffer) {
        ImagePacket packet;
        size_t offset = 0;

        // 提取頭部信息
        std::memcpy(&packet.format, buffer.data() + offset, sizeof(packet.format));
        offset += sizeof(packet.format);
        std::memcpy(&packet.width, buffer.data() + offset, sizeof(packet.width));
        offset += sizeof(packet.width);
        std::memcpy(&packet.height, buffer.data() + offset, sizeof(packet.height));
        offset += sizeof(packet.height);
        std::memcpy(&packet.stride, buffer.data() + offset, sizeof(packet.stride));
        offset += sizeof(packet.stride);
        std::memcpy(&packet.timestamp, buffer.data() + offset, sizeof(packet.timestamp));
        offset += sizeof(packet.timestamp);

        // 提取圖像數據
        packet.data.resize(buffer.size() - offset);
        std::memcpy(packet.data.data(), buffer.data() + offset, packet.data.size());

        return packet;
    }
};

//同步參數
enum CameraType {
    Master,
    Sub,
    Alone,
};
struct CameraReturnStruct
{
    bool start_up_success;
    std::string serial_str;
    k4a_calibration_t calibration;
};

void listenForServer(std::string& serverIP, int& serverPort);

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

int CameraStartup(k4a_device_t &device, std::string &serial_str, k4a_calibration_t &calibration, k4a_device_configuration_t config) {
    uint32_t count = k4a_device_get_installed_count();
    if (count == 0)
    {
        std::cerr << "No k4a devices attached!\n" << std::endl;
        return -1;
    }

    // Open the first plugged in Kinect device
    uint32_t camnum = K4A_DEVICE_DEFAULT;
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
    return 1;
}

int main() {
    bool Stop = false;
    WSADATA wsaData;
    int iResult;

    // 初始化 Winsock
    iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        std::cerr << "WSAStartup failed: " << iResult << std::endl;
        return 1;
    }

    std::string serverIP;
    int serverPort = 0;

    // 监听服务器广播，获取服务器 IP 和端口
    listenForServer(serverIP, serverPort);

    if (serverIP.empty() || serverPort == 0) {
        std::cerr << "Failed to receive server info." << std::endl;
        WSACleanup();
        return 1;
    }

    std::cout << "Received server info: IP=" << serverIP << ", Port=" << serverPort << std::endl;

    // 创建套接字
    SOCKET ConnectSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
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

    std::cout << "Connected to server." << std::endl;

    try {
        //傳送Client相機類別
        int camtype = Alone;
        std::vector<char> camtype_data(sizeof(camtype));
        std::memcpy(camtype_data.data(), &camtype, sizeof(camtype));
        sendMessage(ConnectSocket, -1, camtype_data);
        //接收相機參數
        k4a_device_configuration_t config = K4A_DEVICE_CONFIG_INIT_DISABLE_ALL;
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
        }
        //設定相機參數
        k4a_device_t device;
        CameraReturnStruct CRS;
        if (CameraStartup(device, CRS.serial_str, CRS.calibration, config) < 0)
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
        }
        /*k4a_device_stop_cameras(device);
        k4a_device_close(device);*/
        // 創建錄製線程
        ThreadSafeQueue<ImagePacket> queue;
        std::thread recordThread([&queue, device, &Stop]() {
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
                if (color_image != NULL)
                {

                    //int color_width = k4a_image_get_width_pixels(color_image);
                    //int color_height = k4a_image_get_height_pixels(color_image);
                    //uint64_t timestamp = k4a_image_get_device_timestamp_usec(color_image);
                    // 将K4A图像转换为OpenCV Mat
                    //cv::Mat color_mat(color_height, color_width, CV_8UC4, (void*)k4a_image_get_buffer(color_image));

                    queue.push(ImagePacket::from_k4a_image(color_image));
                    // 释放彩色图像
                    k4a_image_release(color_image);
                }

                // 释放捕获
                k4a_capture_release(capture);
            }
            k4a_device_stop_cameras(device);
            k4a_device_close(device);
            });
        // 創建圖像資料發送線程
        std::thread recordSendThread([&queue, ConnectSocket, &Stop]() {
            while (!Stop) {
                // 從隊列中取出 FrameData
                ImagePacket frameData = queue.wait_and_pop();
                sendMessage(ConnectSocket, 3, frameData.serialize_to_char());

                /*std::vector<char> frameData_timestamp_data(sizeof(frameData.timestamp));
                std::memcpy(frameData_timestamp_data.data(), &frameData.timestamp, sizeof(frameData.timestamp));
                sendMessage(ConnectSocket, 3, frameData_timestamp_data);*/

                /*std::vector<uchar> buffer;
                cv::imencode(".png", frameData.image, buffer);
                std::vector<char> char_vector(buffer.begin(), buffer.end());
                sendMessage(ConnectSocket, 4, char_vector);*/

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
            });
        // 创建接收和发送线程
        std::thread recvThread([ConnectSocket, &Stop]() {
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
                    }
                }
                else {
                    std::cerr << "Unknown message type from server: " << msgType << std::endl;
                }
            }
            });

        //std::thread sendThread([ConnectSocket, &Stop]() {


        //    while (!Stop) {
        //        // 从控制台读取要发送的消息
        //        std::cout << "Enter message to send to server (type 'file:<filepath>' to send a file): ";
        //        std::string input;
        //        std::getline(std::cin, input);

        //        if (input.substr(0, 5) == "file:") {
        //            std::string filePath = input.substr(5);
        //            std::ifstream inFile(filePath, std::ios::binary | std::ios::ate);
        //            if (!inFile) {
        //                std::cerr << "Failed to open file: " << filePath << std::endl;
        //                continue;
        //            }
        //            std::streamsize dataSize = inFile.tellg();
        //            inFile.seekg(0, std::ios::beg);

        //            std::vector<char> data(dataSize);
        //            if (!inFile.read(data.data(), dataSize)) {
        //                std::cerr << "Failed to read file content." << std::endl;
        //                continue;
        //            }
        //            inFile.close();

        //            sendMessage(ConnectSocket, 2, data);
        //            std::cout << "File sent to server" << std::endl;
        //        }
        //        else {
        //            std::vector<char> data(input.begin(), input.end());
        //            sendMessage(ConnectSocket, 1, data);
        //            std::cout << "Text message sent to server" << std::endl;
        //        }
        //    }
        //    });

        recvThread.join();
        //sendThread.join();
        recordSendThread.join();
        recordThread.join();
    }
    catch (const std::exception& e) {
        std::cerr << "Disconnected from server: " << e.what() << std::endl;
    }

    // 关闭套接字
    closesocket(ConnectSocket);
    WSACleanup();
    return 0;
}

void listenForServer(std::string& serverIP, int& serverPort) {
    SOCKET ListenSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ListenSocket == INVALID_SOCKET) {
        std::cerr << "UDP socket failed: " << WSAGetLastError() << std::endl;
        return;
    }

    sockaddr_in recvAddr;
    recvAddr.sin_family = AF_INET;
    recvAddr.sin_port = htons(BROADCAST_PORT);
    recvAddr.sin_addr.s_addr = INADDR_ANY;

    // 绑定套接字到广播端口
    if (bind(ListenSocket, (SOCKADDR*)&recvAddr, sizeof(recvAddr)) == SOCKET_ERROR) {
        std::cerr << "UDP bind failed: " << WSAGetLastError() << std::endl;
        closesocket(ListenSocket);
        return;
    }

    std::cout << "Listening for server broadcast on UDP port " << BROADCAST_PORT << "..." << std::endl;

    char recvBuffer[256];
    sockaddr_in senderAddr;
    int senderAddrSize = sizeof(senderAddr);

    // 设置超时时间
    int timeout = 30000; // 30秒
    setsockopt(ListenSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    int recvResult = recvfrom(ListenSocket, recvBuffer, sizeof(recvBuffer) - 1, 0, (SOCKADDR*)&senderAddr, &senderAddrSize);
    if (recvResult == SOCKET_ERROR) {
        std::cerr << "recvfrom failed: " << WSAGetLastError() << std::endl;
        closesocket(ListenSocket);
        return;
    }

    recvBuffer[recvResult] = '\0';

    // 解析接收到的服务器信息
    std::string message(recvBuffer);
    if (message.find("SERVER_INFO:") == 0) {
        size_t pos1 = message.find(':', 12);
        size_t pos2 = message.find(':', pos1 + 1);
        if (pos1 != std::string::npos && pos2 != std::string::npos) {
            serverIP = message.substr(12, pos1 - 12);
            serverPort = std::stoi(message.substr(pos1 + 1));
        }
    }

    closesocket(ListenSocket);
}
