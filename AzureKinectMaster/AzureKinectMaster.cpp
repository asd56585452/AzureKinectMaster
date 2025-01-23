#include <boost/asio.hpp>
#include <iostream>
#include <k4a/k4a.h>
#include <k4arecord/record.h>
#include <k4arecord/playback.h>

/*
連線到伺服器
*/

using boost::asio::ip::tcp;

boost::asio::io_context io_context;
tcp::socket HOST(io_context);
k4a_device_t device;
uint32_t camnum = 0;
std::string camera_name = "";
int camera_id = -1;

void Print_error(std::string s);
int Connect_to_host();
int Get_camera();
int Recvive_camera_id();
int ReceiveString(std::string& client_message);
int SendString(std::string& host_message);

int main() {
    if (Connect_to_host() == 1)
    {
        Print_error("Connect_to_host");
        return 1;
    }
    /*if (Get_camera() == 1)
    {
        Print_error("Get_camera");
        return 1;
    }*/
    if (Recvive_camera_id() == 1)
    {
        Print_error("Recvive_camera_id");
        return 1;
    }
    return 0;
}

void Print_error(std::string s) {
    std::cerr << s << " has error" << std::endl;
}

int Connect_to_host()
{
    try {
        HOST.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 8080));
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
        std::string s = "0123456";
        SendString(s);
        return 0;
    }
    catch (...) {
        return 1;
    }
}

int ReceiveString(std::string& client_message)
{
    try
    {
        // 接收客戶端的數據
        boost::asio::streambuf buffer;
        boost::asio::read_until(HOST, buffer, "\n");

        // 解析數據
        std::istream input_stream(&buffer);
        std::getline(input_stream, client_message);
        return 0;
    }
    catch (const std::exception&)
    {
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
    catch (const std::exception&)
    {
        return 1;
    }

}
