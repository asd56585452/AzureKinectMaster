#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>

#pragma comment(lib, "Ws2_32.lib")

#define BROADCAST_PORT 8888   // 与服务器广播端口一致

void listenForServer(std::string& serverIP, int& serverPort);

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

    // 读取图片文件到缓冲区
    std::ifstream inFile("send_image.png", std::ios::binary | std::ios::ate);
    if (!inFile) {
        std::cerr << "Failed to open file: send_image.png" << std::endl;
        closesocket(ConnectSocket);
        WSACleanup();
        return 1;
    }

    std::streamsize dataSize = inFile.tellg();
    inFile.seekg(0, std::ios::beg);

    if (dataSize <= 0) {
        std::cerr << "File is empty or error occurred." << std::endl;
        inFile.close();
        closesocket(ConnectSocket);
        WSACleanup();
        return 1;
    }

    std::vector<char> dataBuffer(dataSize);
    if (!inFile.read(dataBuffer.data(), dataSize)) {
        std::cerr << "Failed to read file content." << std::endl;
        inFile.close();
        closesocket(ConnectSocket);
        WSACleanup();
        return 1;
    }
    inFile.close();

    // 发送数据大小
    int netDataSize = htonl(static_cast<int>(dataSize));
    iResult = send(ConnectSocket, (char*)&netDataSize, sizeof(int), 0);
    if (iResult == SOCKET_ERROR) {
        std::cerr << "send failed (data size): " << WSAGetLastError() << std::endl;
        closesocket(ConnectSocket);
        WSACleanup();
        return 1;
    }

    // 发送实际数据
    int totalSent = 0;
    while (totalSent < dataSize) {
        iResult = send(ConnectSocket, dataBuffer.data() + totalSent, static_cast<int>(dataSize) - totalSent, 0);
        if (iResult == SOCKET_ERROR) {
            std::cerr << "send failed (data): " << WSAGetLastError() << std::endl;
            closesocket(ConnectSocket);
            WSACleanup();
            return 1;
        }
        totalSent += iResult;
    }

    std::cout << "Data sent successfully." << std::endl;

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
    int timeout = 10000; // 10秒
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
            serverPort = std::stoi(message.substr(pos1 + 1, pos2 - pos1 - 1));
        }
    }

    closesocket(ListenSocket);
}
