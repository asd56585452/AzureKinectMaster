#include <boost/asio.hpp>
#include <iostream>
#include <filesystem> 
#include <fstream>
#include <k4a/k4a.h>
#include <k4arecord/record.h>
#include <k4arecord/playback.h>
#include <thread>
#include <boost/process.hpp>
#include <filesystem> // For file deletion
#include <atomic>

namespace fs = std::filesystem;


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
int Preview_streaming();
void StopPreview();
void StartPreview();
void IDConfirm();
void StartPreviewControl();
void StopPreviewControl();

//use to control preview stream
std::atomic<bool> preview_running(false);
std::atomic<bool> preview_control_running(false);
std::atomic<bool> streaming(false);
std::atomic<bool> preview_socket_alive(true);

std::thread preview_thread;
std::thread preview_control_thread;
boost::asio::io_context io_context_preview;
tcp::socket preview_socket(io_context_preview);
tcp::resolver resolver(io_context_preview);
char cmd_buffer;
#define CHECK_AND_RETURN(func) \
    if ((func) == 1) { \
        Print_error(#func); \
        StopPreview(); \
        StopPreviewControl();\
        return 1; \
    }
int main(int argc, char* argv[]) {
    if (argc > 1) {
        HOSTIP = std::string(argv[1]);
    }

    CHECK_AND_RETURN(Connect_to_host());
    CHECK_AND_RETURN(Get_camera());
    CHECK_AND_RETURN(Switch_working_environments());
    CHECK_AND_RETURN(Recvive_camera_id());
    boost::asio::connect(preview_socket, resolver.resolve(HOSTIP, "13579"));
    IDConfirm();
    StartPreviewControl();
    while (true)
    {
        // 先停止 preview 再跑錄影指令
        CHECK_AND_RETURN(Commands_recvive());

        // 傳送錄好的檔案
        CHECK_AND_RETURN(Send_mkv_file());
    }

    // 若有離開迴圈，安全停止 preview thread
    StopPreview();
    StopPreviewControl();
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
        //k4a_device_close(device);
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
        //std::this_thread::sleep_for(std::chrono::seconds(10));
        std::cout << "Received command: [" << command << "]\n";
        bp::opstream input; // 用於提供標準輸入
        bp::ipstream output; // 用於接收標準輸出
        
        bp::child process(command, bp::std_out > output);
        std::string line;
        if (camera_id != 1)
        {
            std::vector<std::string> subordinate_checklists = { "Device serial number: " + camera_name + "\r","Device version: Rel; C: 1.6.110; D: 1.6.80[6109.7]; A: 1.6.14\r","Device started\r","[subordinate mode] Waiting for signal from master\r" };
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
            std::vector<std::string> master_checklists = { "Device serial number: " + camera_name + "\r","Device version: Rel; C: 1.6.110; D: 1.6.80[6109.7]; A: 1.6.14\r","Device started\r" };
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
        std::string msg = e.what();
        if (msg.find("End of file") != std::string::npos) {
            // detect EOF error, treat as connection closed or handled case
            std::cerr << "Detected EOF. Treating as graceful close.\n";
            return 0; // or return a special code if you want to handle reconnection
        }
        return 1;
    }

}

int SendString(std::string& host_message)
{
    try
    {
        boost::asio::write(HOST, boost::asio::buffer(host_message + "\n"));
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

        file.close();


        // 顯式關閉socket
        socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both);
        socket.close();

        if (fs::exists(file_path)) {
            fs::remove(file_path);
            std::cout << "File deleted successfully!\n";
        }
        else {
            std::cerr << "Warning: File not found for deletion: " << file_path << "\n";
        }

        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
void start_async_read_command() {
    preview_socket.async_read_some(boost::asio::buffer(&cmd_buffer, 1),
        [](boost::system::error_code ec, std::size_t length) {
            if (!preview_socket_alive.load()) {
                std::cout << "[Control] Socket is no longer alive. Exiting callback.\n";
                return;
            }

            if (!ec && length == 1) {
                char cmd = cmd_buffer;
                if (cmd == 'S') {
                    streaming = true;
                    std::cout << "[Control] Start streaming command received.\n";
                }
                else if (cmd == 'P') {
                    streaming = false;
                    std::cout << "[Control] Pause streaming command received.\n";
                }
                else if (cmd == 'R') {
                    std::cout << "[Control] Prepare for recording.\n";
                    StopPreview();
                }
                else if (cmd == 'O') {
                    std::cout << "[Control] Open Previewing Thread.\n";
                    StartPreview();
                }
                else {
                    std::cout << "[Control] Unknown command received: " << cmd << "\n";
                }
            }
            else if (ec) {
                std::cerr << "[Control] Async read error: " << ec.message() << "\n";

                if (ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset) {
                    std::cerr << "[Control] Connection closed by peer.\n";
                }

                preview_control_running = false;
                preview_socket_alive = false;

                // Cancel socket to abort any pending operations
                boost::system::error_code cancel_ec;
                preview_socket.cancel(cancel_ec);

                return;
            }

            if (preview_control_running && preview_socket_alive) {
                // 繼續監聽
                start_async_read_command();
            }
        });
}


// preview control thread 入口函式
void PreviewControlThreadFunc() {
    // 開始非同步監聽
    start_async_read_command();

    // io_context 執行循環（阻塞直到 io_context 被 stop）
    io_context_preview.run();
}
void IDConfirm() {
    try {
        char frame_type = 'R'; // or a special type if you define one for preframe
        uint32_t index = static_cast<uint32_t>(camera_id);
        uint32_t image_size = 0;

        boost::asio::write(preview_socket, boost::asio::buffer(&frame_type, 1));
        boost::asio::write(preview_socket, boost::asio::buffer(&index, 4));
        boost::asio::write(preview_socket, boost::asio::buffer(&image_size, 4));

        std::cout << "Sent preframe for ID confirmation: camera_id=" << camera_id << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "Error sending preframe: " << e.what() << std::endl;
    }
}
int Preview_streaming() {
    try {
        k4a_device_open(camnum, &device);
        // 設定並啟動攝影機
        k4a_device_configuration_t config = K4A_DEVICE_CONFIG_INIT_DISABLE_ALL;
        config.color_format = K4A_IMAGE_FORMAT_COLOR_MJPG;
        config.color_resolution = K4A_COLOR_RESOLUTION_720P;
        config.depth_mode = K4A_DEPTH_MODE_NFOV_UNBINNED;
        config.camera_fps = K4A_FRAMES_PER_SECOND_30;

        if (K4A_FAILED(k4a_device_start_cameras(device, &config))) {
            std::cerr << "Failed to start cameras\n";
            return 0;
        }

        while (preview_running) {
            if (streaming) {
                k4a_capture_t capture;
                if (k4a_device_get_capture(device, &capture, 1000) == K4A_WAIT_RESULT_SUCCEEDED) {

                    k4a_image_t color_image = k4a_capture_get_color_image(capture);
                    if (color_image != NULL) {
                        uint8_t* buffer = k4a_image_get_buffer(color_image);
                        int size = k4a_image_get_size(color_image);

                        char frame_type = 'R';
                        boost::asio::write(preview_socket, boost::asio::buffer(&frame_type, 1));
                        uint32_t index = static_cast<uint32_t>(camera_id);
                        boost::asio::write(preview_socket, boost::asio::buffer(&index, 4));
                        uint32_t image_size = static_cast<uint32_t>(size);
                        boost::asio::write(preview_socket, boost::asio::buffer(&image_size, 4));

                        boost::asio::write(preview_socket, boost::asio::buffer(buffer, size));

                        k4a_image_release(color_image);
                    }
                    k4a_capture_release(capture);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 FPS
        }

        // 停止攝影機
        k4a_device_stop_cameras(device);
        
        std::cout << "Preview stopped." << std::endl;

    }
    catch (const std::exception& e) {
        std::cerr << "Preview streaming error: " << e.what() << std::endl;

    }
}

void StartPreview() {
    if (preview_running)return;
    preview_running = true;
    preview_thread = std::thread(Preview_streaming);
    
}

// 停止 preview
void StopPreview() {
    if (!preview_running)return;

    preview_running = false;
    if (preview_thread.joinable())
        preview_thread.join();
    // 關閉相機
    k4a_device_close(device);
}
void StartPreviewControl() {
    if (preview_control_running)return;
    preview_control_running = true;
    io_context_preview.restart();
    preview_control_thread = std::thread(PreviewControlThreadFunc);
}
void StopPreviewControl() {
    preview_control_running = false;
    preview_socket_alive = false;

    // Cancel all async operations before stopping
    boost::system::error_code ec;
    preview_socket.cancel(ec);
    if (ec) {
        std::cerr << "Error cancelling preview_socket: " << ec.message() << std::endl;
    }

    io_context_preview.stop();

    if (preview_control_thread.joinable())
        preview_control_thread.join();
    // Finally close socket
    preview_socket.close(ec);
    if (ec) {
        std::cerr << "Error closing preview_socket: " << ec.message() << std::endl;
    }
}
