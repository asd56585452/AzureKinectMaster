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

#pragma comment(lib, "Ws2_32.lib")

#define BROADCAST_PORT 8888   // 与服务器广播端口一致

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
        struct CameraReturnStruct CRS;
        if (CameraStartup(device, CRS.serial_str, CRS.calibration, config) < 0)
        {
            std::cerr << "Failed to Startup camera" << std::endl;
            CRS.start_up_success = false;
        }
        else
        {
            CRS.start_up_success = true;
        }
        // 回報設定成功
        std::vector<char> CRS_data(sizeof(CRS));
        std::memcpy(CRS_data.data(), &CRS, sizeof(CRS));
        sendMessage(ConnectSocket, -3, CRS_data);
        // 创建接收和发送线程
        std::thread recvThread([ConnectSocket]() {
            while (true) {
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
                else {
                    std::cerr << "Unknown message type from server: " << msgType << std::endl;
                }
            }
            });

        std::thread sendThread([ConnectSocket]() {


            while (true) {
                // 从控制台读取要发送的消息
                std::cout << "Enter message to send to server (type 'file:<filepath>' to send a file): ";
                std::string input;
                std::getline(std::cin, input);

                if (input.substr(0, 5) == "file:") {
                    std::string filePath = input.substr(5);
                    std::ifstream inFile(filePath, std::ios::binary | std::ios::ate);
                    if (!inFile) {
                        std::cerr << "Failed to open file: " << filePath << std::endl;
                        continue;
                    }
                    std::streamsize dataSize = inFile.tellg();
                    inFile.seekg(0, std::ios::beg);

                    std::vector<char> data(dataSize);
                    if (!inFile.read(data.data(), dataSize)) {
                        std::cerr << "Failed to read file content." << std::endl;
                        continue;
                    }
                    inFile.close();

                    sendMessage(ConnectSocket, 2, data);
                    std::cout << "File sent to server" << std::endl;
                }
                else {
                    std::vector<char> data(input.begin(), input.end());
                    sendMessage(ConnectSocket, 1, data);
                    std::cout << "Text message sent to server" << std::endl;
                }
            }
            });

        recvThread.join();
        sendThread.join();
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
