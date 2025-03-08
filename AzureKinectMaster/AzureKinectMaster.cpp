#include <boost/asio.hpp>
#include <iostream>
#include <filesystem> 
#include <fstream>
#include <k4a/k4a.h>
#include <k4arecord/record.h>
#include <k4arecord/playback.h>
#include <thread>
#include <boost/process.hpp>

/*
連線到伺服器
*/ 

using boost::asio::ip::tcp;

boost::asio::io_context io_context;
tcp::socket HOST(io_context);
k4a_device_t device;
uint32_t camnum = 0;
std::string camera_name = "000835513412";
int camera_id = -1;
std::string mkvfilename = "output.mkv";
std::string HOSTIP = "192.168.50.200";//140.114.24.234,127.0.0.1 

void Print_error(std::string s);
int Switch_working_environments();
int Connect_to_host();
int Get_camera();
int Recvive_camera_id();
int Commands_recvive();
std::string get_last_word(const std::string& str);
int ReceiveString(std::string& client_message);
int SendString(std::string& host_message);
int Send_mkv_file();
int send_file(const std::string& file_path, const std::string& host, unsigned short port);

#define CHECK_AND_RETURN(func) \
    if ((func) == 1) { \
        Print_error(#func); \
        return 1; \
    }

int main() {
    CHECK_AND_RETURN(Connect_to_host());
    CHECK_AND_RETURN(Get_camera());
    CHECK_AND_RETURN(Switch_working_environments());
    CHECK_AND_RETURN(Recvive_camera_id());
    while (true)
    {
        CHECK_AND_RETURN(Commands_recvive());
        CHECK_AND_RETURN(Send_mkv_file());
    }
    
    return 0;
}

void Print_error(std::string s) {
    std::cerr << s << " has error" << std::endl;
}

int Switch_working_environments()//Create directories and switch working environments by camera_name
{
    try
    {
        if (!std::filesystem::exists(camera_name))
            std::filesystem::create_directory(camera_name);
        std::filesystem::current_path(camera_name);
        return 0;
    }
    catch (...)
    {
        return 1;
    }
}

int Connect_to_host()
{
    try {
        HOST.connect(tcp::endpoint(boost::asio::ip::make_address(HOSTIP), 8080));
        return 0;
    }
    catch (...) {
        return 1;
    }
}

int Get_camera() 
{
    try
    {
        uint32_t count = k4a_device_get_installed_count();
        if (count == 0)
        {
            std::cerr << "No k4a devices attached!\n" << std::endl;
            return 1;
        }
        // Open the first plugged in Kinect device
        camnum = 0;
        while (K4A_FAILED(k4a_device_open(camnum, &device)))
        {
            camnum++;
            if (camnum >= count)
            {
                std::cerr << "Failed to open k4a device!\n" << std::endl;
                return 1;
            }
        }
        size_t serial_size = 0;
        k4a_device_get_serialnum(device, NULL, &serial_size);

        // Allocate memory for the serial, then acquire it
        char* serial = (char*)(malloc(serial_size));
        k4a_device_get_serialnum(device, serial, &serial_size);
        camera_name.assign(serial);
        std::cout << "Opened device: " << camera_name << "\n";
        return 0;
    }
    catch (...)
    {
        return 1;
    }
}

int Recvive_camera_id()
{
    try {
        std::string s = camera_name;
        CHECK_AND_RETURN(SendString(s));
        s = std::to_string(camnum);
        CHECK_AND_RETURN(SendString(s));
        s = "";
        CHECK_AND_RETURN(ReceiveString(s));
        camera_id = std::stoi(s);
        k4a_device_close(device);
        return 0;
    }
    catch (...) {
        return 1;
    }
}

std::string get_last_word(const std::string& str) {
    size_t pos = str.find_last_of(' '); // 找到最後一個空白的索引
    if (pos == std::string::npos) {
        return str; // 如果沒有空白，整個字串就是最後的單詞
    }
    return str.substr(pos + 1); // 截取從最後一個空白到結尾的子字串
}

namespace bp = boost::process;

int Commands_recvive()
{
    try {
        std::string command;
        CHECK_AND_RETURN(ReceiveString(command));
        std::cout << command << std::endl;
        bp::opstream input; // 用於提供標準輸入
        bp::ipstream output; // 用於接收標準輸出
        bp::child process(command, bp::std_out > output);
        std::string line;
        if (camera_id != 1)
        {
            std::vector<std::string> subordinate_checklists = { "Device serial number: " + camera_name+"\r","Device version: Rel; C: 1.6.110; D: 1.6.80[6109.7]; A: 1.6.14\r","Device started\r","[subordinate mode] Waiting for signal from master\r"};
            int checki = 0;
            for (checki = 0; std::getline(output, line);) {
                std::cout << line << '\n';
                if (line != subordinate_checklists[checki])
                {
                    break;
                }
                checki++;
                if (checki >= subordinate_checklists.size())
                    break;
            }
            if (checki != subordinate_checklists.size())
            {
                std::string s = "fail";
                CHECK_AND_RETURN(SendString(s));
                return 1;
            }
            else
            {
                std::string s = "success";
                CHECK_AND_RETURN(SendString(s));
            }
        }
        else
        {
            std::vector<std::string> master_checklists = { "Device serial number: " + camera_name + "\r","Device version: Rel; C: 1.6.110; D: 1.6.80[6109.7]; A: 1.6.14\r","Device started\r"};
            int checki = 0;
            for (checki = 0; std::getline(output, line);) {
                std::cout << line << '\n';
                if (line != master_checklists[checki])
                {
                    break;
                }
                checki++;
                if (checki >= master_checklists.size())
                    break;
            }
            if (checki != master_checklists.size())
            {
                std::string s = "fail";
                CHECK_AND_RETURN(SendString(s));
                return 1;
            }
            else
            {
                std::string s = "success";
                CHECK_AND_RETURN(SendString(s));
            }
        }
        while (std::getline(output, line))
        {
            std::cout << line << '\n';
        }
        std::cout << "Exit code: " << process.exit_code() << '\n';
        process.wait();
        mkvfilename = get_last_word(command);
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}

int Send_mkv_file()
{
    try {
        std::string s;
        CHECK_AND_RETURN(ReceiveString(s));
        if (s != "RequestFile")
        {
            std::cerr << "Not RequestFile" << std::endl;
            return 1;
        }
        send_file(mkvfilename, HOSTIP, 12345);
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}

void ReadUntilNewline(boost::asio::ip::tcp::socket& socket, boost::asio::streambuf& buffer, char c)
{
    char ch;  // 用來儲存每次讀取的字元

    while (true)
    {
        boost::asio::read(socket, boost::asio::buffer(&ch, 1));  // 每次讀取一個字元
        buffer.sputc(ch);  // 將字元寫入 streambuf

        if (ch == c)  // 如果讀到換行符，則停止
        {
            break;
        }
    }
}

int ReceiveString(std::string& client_message)
{
    try
    {
        // 接收客戶端的數據
        boost::asio::streambuf buffer;
        ReadUntilNewline(HOST, buffer, '\n');

        // 解析數據
        std::istream input_stream(&buffer);
        std::getline(input_stream, client_message);
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

}

int SendString(std::string& host_message)
{
    try
    {
        boost::asio::write(HOST, boost::asio::buffer(host_message+"\n"));
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

}

int send_file(const std::string& file_path, const std::string& host, unsigned short port) {
    try {
        boost::asio::io_context io_context;
        tcp::socket socket(io_context);
        tcp::resolver resolver(io_context);
        boost::asio::connect(socket, resolver.resolve(host, std::to_string(port)));

        std::ifstream file(file_path, std::ios::binary | std::ios::ate);
        if (!file) {
            std::cerr << "Failed to open file: " << file_path << "\n";
            return 1;
        }

        std::streamsize file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        // 發送檔案大小
        boost::asio::write(socket, boost::asio::buffer(&file_size, sizeof(file_size)));

        const size_t chunk_size = 4096;
        char buffer[chunk_size];
        std::streamsize bytes_read;

        while ((bytes_read = file.read(buffer, chunk_size).gcount()) > 0) {
            boost::asio::write(socket, boost::asio::buffer(buffer, bytes_read));
        }

        std::cout << "File sent successfully!\n";

        // 顯式關閉socket
        socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both);
        socket.close();
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}