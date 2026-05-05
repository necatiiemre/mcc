#include "DviManager.h"
#include "DirectoryManager.h"
#include <opencv2/opencv.hpp>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <iostream>
#include <thread>
#include <sched.h>
#include <pthread.h>
#include <ssim_gpu.h>
#include "ImageProcessor.h"
#include "Resolution.h"
#include "DebugLog.h"
#include <cstdio>
#include <memory>
#include <array>
#include <regex>
#include <queue>
#include <condition_variable>
#include "Globals.h"
#include "ErrorUtils.h"
#include <opencv2/core/ocl.hpp>
#include <poll.h>

// *********************************************************************************
// HATA DÜZELTME 1: ThreadSafeQueue tanımını .cpp dosyasının en üstüne taşıyoruz.
// Bu, derleyicinin DviManager metodlarını derlemeden önce sınıfın tam
// tanımını bilmesini sağlar ve "incomplete type" hatasını çözer.
// *********************************************************************************
template <typename T>
class ThreadSafeQueue
{
private:
    mutable std::mutex mtx;
    std::queue<T> data_queue;
    std::condition_variable cv;

public:
    void push(T new_value)
    {
        std::lock_guard<std::mutex> lock(mtx);
        data_queue.push(std::move(new_value));
        cv.notify_one();
    }

    bool wait_and_pop(T &value)
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]
                { return !data_queue.empty(); });
        value = std::move(data_queue.front());
        data_queue.pop();
        return true;
    }
    bool pop(T &value)
    {
        value = std::move(data_queue.front());
        data_queue.pop();
        return true;
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mtx);
        return data_queue.empty();
    }

    void notify_all()
    {
        cv.notify_all();
    }
};

namespace
{
    const char *dviDeviceChannel1 = "/dev/video0";
    const char *dviDeviceChannel2 = "/dev/video1";
    const int WIDTH = 1920;
    const int HEIGHT = 1080;
    const int PIXEL_FORMAT = V4L2_PIX_FMT_YUYV;

    void setThreadAffinity(int coreId)
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(coreId, &cpuset);
        pthread_t thread = pthread_self();

        int result = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
        if (result != 0)
        {
            LOG_ERROR("Failed to set thread affinity for core " << coreId << ". errno=" << result);
            // Optional: fallback or skip instead of aborting
        }
    }

}

DviManager::DviManager()
{
    frameQueueCh1 = std::make_unique<ThreadSafeQueue<std::unique_ptr<cv::Mat>>>();
    frameQueueCh2 = std::make_unique<ThreadSafeQueue<std::unique_ptr<cv::Mat>>>();
    displayQueueCh1 = std::make_unique<ThreadSafeQueue<std::unique_ptr<cv::Mat>>>();
    displayQueueCh2 = std::make_unique<ThreadSafeQueue<std::unique_ptr<cv::Mat>>>();
}

DviManager::~DviManager()
{
    stop(2);
}

uint8_t DviManager::producer_worker(const char *device, int channelId, int coreId1)
{
    setThreadAffinity(coreId1);

    int fd = -1;
    bool boolResult;

    // Kanalın buffer vektörü
    auto &buffers = (channelId == 0) ? buffersCh1 : buffersCh2;

    rc = initDevice(device, fd, buffers, channelId, boolResult);
    if (!boolResult)
    {
        LOG_ERROR("Producer failed to initialize device: " << device);
        if (channelId == 0)
            runningCh1 = false;
        else
            runningCh2 = false;
        return CODE_DVI_MANAGER_INIT_DEVICE_FAILED;
    }

    // poll() için yapı
    struct pollfd fds;
    fds.fd = fd;
    fds.events = POLLIN;

    while (!stopRequestedArray[channelId])
    {
        // Frame hazır mı bekle
        int ret = poll(&fds, 1, 10); // 100 ms timeout (0.1 saniye)

        if (ret == -1)
        {
            if (errno == EINTR)
                continue; // Sinyal geldiyse devam et
            LOG_ERROR("poll() error on device: " << device << " errno=" << errno);
            break;
        }
        else if (ret == 0)
        {
            // Timeout oldu, yeni frame gelmedi
            continue;
        }

        // Frame hazır, al
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0)
        {
            LOG_ERROR("Producer failed to dequeue buffer for " << device << " errno=" << errno);
            break;
        }

        // Doğru buffer'ı seç
        auto &buffer = buffers[buf.index];

        auto rgbFrame = std::make_unique<cv::Mat>();
        cv::Mat yuyv(HEIGHT, WIDTH, CV_8UC2, buffer.start);
        cv::cvtColor(yuyv, *rgbFrame, cv::COLOR_YUV2BGR_YUYV);

        // Buffer'ı tekrar sıraya koy
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
        {
            LOG_ERROR("Producer failed to queue buffer for " << device);
        }

        if (channelId == 0)
        {
            frameQueueCh1->push(std::move(rgbFrame));
        }
        else
        {
            frameQueueCh2->push(std::move(rgbFrame));
        }
    }

    cleanup(fd, buffers);
    LOG_INFO("Producer for " << device << " has stopped.");
    return CODE_SUCCESS;
}

uint8_t DviManager::consumer_worker(int channelId, int core)
{
    setThreadAffinity(core);
    directory_manager.createDviDirectories();
    std::string errorPath, videoPath, time_stamp, videoFile;
    rc = directory_manager.getDviErrorPath(channelId + 1, errorPath);
    rc = directory_manager.getDviVideoPath(channelId + 1, videoPath);
    rc = getCurrentTimestamp(time_stamp);
    videoFile = videoPath + "/" + time_stamp + ".avi";

    FILE *ffmpeg_pipe = startFFmpegWriter(videoFile, width_1920, height_1080);

    DviChannel &channel = (channelId == 0) ? channel_1 : channel_2;

    // auto &displayQueue = (channelId == 0) ? *displayQueueCh1 : *displayQueueCh2;

    resetStatistics(channelId);

    float prevScore = 0.0f;
    bool firstFrame = true;

    getDeviceTemperature(channelId, channel);
    const auto start_time = std::chrono::steady_clock::now();

    cv::Mat currentGray;
    cv::Mat currentFrame;
    cv::Mat prevGray;

    while (isRunning)
    {
        std::unique_ptr<cv::Mat> currentFramePtr;
        if (!frameQueue.wait_and_pop(currentFramePtr))
        {
            continue;
        }
        currentFrame = std::move(*currentFramePtr); // shallow copy + move, hızlı

        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastTempUpdate).count() >= 60)
        {
            getDeviceTemperature(channelId, temperature);
            lastTempUpdate = now;
        }

        // SSIM ve hata tespiti
        cv::cvtColor(currentFrame, currentGray, cv::COLOR_BGR2GRAY);

        if (firstFrame)
        {
            std::swap(prevGray, currentGray);

            firstFrame = false;
        }
        else
        {
            float score = ssimGPU(prevGray, currentGray);
            isErrorFrame(prevScore, score, currentFrame, errorPath, frameCounter, errorCounter);
            prevGray = currentGray.clone();
            prevScore = score;
        }


        // drawDviStats(currentFrame,)

        cv::putText(currentFrame, "Time: " + std::string(timeStr), cv::Point(10, 30),
                    cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);
        cv::putText(currentFrame, "FPS: " + std::to_string(static_cast<int>(currentFps)), cv::Point(10, 60),
                    cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);
        cv::putText(currentFrame, "Frames: " + std::to_string(frameCounter), cv::Point(10, 90),
                    cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);
        cv::putText(currentFrame, "Errors: " + std::to_string(errorCounter), cv::Point(10, 120),
                    cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 255), 2);

        int roundedTemp = static_cast<int>(std::round(temperature));
        cv::Scalar tempColor = (roundedTemp >= 85) ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0);
        cv::putText(currentFrame, "Temp: " + std::to_string(roundedTemp) + " C",
                    cv::Point(10, 150), cv::FONT_HERSHEY_SIMPLEX, 0.7,
                    tempColor, 2);

        writeFFmpegFrame(ffmpeg_pipe, currentFrame);

        auto displayFrame = std::make_unique<cv::Mat>(currentFrame);

        if (channelId == 0)
            displayQueueCh1->push(std::move(displayFrame));
        else
            displayQueueCh2->push(std::move(displayFrame));
    }

    fflush(ffmpeg_pipe);
    int exitCode = pclose(ffmpeg_pipe);
    ffmpeg_pipe = nullptr;
    LOG_INFO("FFmpeg pipe closed with exit code: " << exitCode);

    LOG_INFO("Consumer for channel " << channelId << " has stopped.");
    return CODE_SUCCESS;
}

void DviManager::display_worker(int channelId, int coreId)
{
    setThreadAffinity(coreId);
    std::string windowName = "DVI Channel " + std::to_string(channelId + 1);
    cv::namedWindow(windowName, cv::WINDOW_AUTOSIZE);

    auto &displayQueue = (channelId == 0) ? *displayQueueCh1 : *displayQueueCh2;

    while (!stopRequestedArray[channelId])
    {
        std::unique_ptr<cv::Mat> frame;
        if (!displayQueue.wait_and_pop(frame))
            continue;

        if (frame && !frame->empty())
        {
            cv::imshow(windowName, *frame);
        }

        if (cv::waitKey(1) >= 0)
        {
            stopRequestedArray[channelId] = true;
            break;
        }
    }

    cv::destroyWindow(windowName);
    cv::waitKey(10);
    LOG_INFO("Display thread for channel " << channelId << " has stopped.");
}

// Diğer metodlar (start, stop, initDevice vb.) aynı kalabilir.
// Sadece stop fonksiyonunda notify_all() çağrılarının doğru çalıştığından emin olalım.
uint8_t DviManager::start(int channel)
{
    try
    {
        cv::setNumThreads(1);         // OpenCV'nin iş parçacığı havuzunu 1'e sabitle
        cv::ocl::setUseOpenCL(false); // Eğer OpenCL kullanıyorsa onu da kapat
        rc = directory_manager.createDviDirectories();
        checkReturnCode(rc, "DVI directory create process failed.");

        if ((channel == 0 || channel == 2) && !runningCh1)
        {
            stopRequestedArray[0] = false;
            runningCh1 = true;
            producerThread1 = std::thread(&DviManager::producer_worker, this, dviDeviceChannel1, 0, 12);
            consumerThread1 = std::thread(&DviManager::consumer_worker, this, 0, 12);
            displayThread1 = std::thread(&DviManager::display_worker, this, 0, 13);
            LOG_INFO("Started DVI Channel 1 (Producer/Consumer).");
        }

        if ((channel == 1 || channel == 2) && !runningCh2)
        {
            stopRequestedArray[1] = false;
            runningCh2 = true;
            producerThread2 = std::thread(&DviManager::producer_worker, this, dviDeviceChannel2, 1, 14);
            consumerThread2 = std::thread(&DviManager::consumer_worker, this, 1, 14);
            displayThread2 = std::thread(&DviManager::display_worker, this, 1, 15);
            LOG_INFO("Started DVI Channel 2 (Producer/Consumer).");
        }
        return CODE_SUCCESS;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("DVI Start Error: " << e.what());
        return CODE_DVI_MANAGER_START_FAILED;
    }
}

uint8_t DviManager::stop(int channel)
{
    try
    {
        cout << "Stop trigger" << endl;
        if ((channel == 0 || channel == 2) && runningCh1.load())
        {
            stopRequestedArray[0] = true;
            runningCh1 = false;
            frameQueueCh1->notify_all();

            if (producerThread1.joinable())
                producerThread1.join();
            if (consumerThread1.joinable())
                consumerThread1.join();
            if (displayThread1.joinable())
                displayThread1.join();

            LOG_INFO("Stopped DVI Channel 1.");
        }

        if ((channel == 1 || channel == 2) && runningCh2.load())
        {
            stopRequestedArray[1] = true;
            runningCh2 = false;
            frameQueueCh2->notify_all();

            if (producerThread2.joinable())
                producerThread2.join();
            if (consumerThread2.joinable())
                consumerThread2.join();
            if (displayThread2.joinable())
                displayThread2.join();

            LOG_INFO("Stopped DVI Channel 2.");
        }

        return CODE_SUCCESS;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("DVI Stop Error: " << e.what());
        return CODE_DVI_MANAGER_STOP_FAILED;
    }
}

uint8_t DviManager::initDevice(
    const char *device,
    int &fd,
    std::vector<Buffer> &buffers,
    int channelId,
    bool &boolResult)
{
    fd = open(device, O_RDWR | O_NONBLOCK);
    if (fd < 0)
    {
        LOG_ERROR("Failed to open device: " << device);
        boolResult = false;
        return CODE_DVI_MANAGER_INIT_DEVICE_FAILED;
    }

    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = WIDTH;
    fmt.fmt.pix.height = HEIGHT;
    fmt.fmt.pix.pixelformat = PIXEL_FORMAT;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        LOG_ERROR("Failed to set format for device: " << device);
        close(fd);
        boolResult = false;
        return CODE_DVI_MANAGER_SET_FORMAT_DEVICE_FAILED;
    }

    struct v4l2_requestbuffers req = {};
    req.count = 8;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0)
    {
        LOG_ERROR("Failed to request buffers for device: " << device);
        close(fd);
        boolResult = false;
        return CODE_DVI_MANAGER_BUFFER_REQUEST_FAILED;
    }

    if (req.count < 2)
    {
        LOG_ERROR("Insufficient buffer memory on device: " << device);
        close(fd);
        boolResult = false;
        return CODE_DVI_MANAGER_INSUFFICIENT_BUFFERS;
    }

    buffers.resize(req.count);

    // Tüm buffer'ları mmap et
    for (size_t i = 0; i < req.count; ++i)
    {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
            LOG_ERROR("Failed to query buffer " << i << " for device: " << device);
            close(fd);
            boolResult = false;
            return CODE_DVI_MANAGER_QUERY_BUFFER_REQUEST_FAILED;
        }

        buffers[i].length = buf.length;
        buffers[i].start = mmap(
            NULL,
            buf.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd,
            buf.m.offset);

        if (buffers[i].start == MAP_FAILED)
        {
            LOG_ERROR("Failed to mmap buffer " << i << " for device: " << device);
            close(fd);
            boolResult = false;
            return CODE_DVI_MANAGER_DEVICE_MMAP_BUFFER_FAILED;
        }
    }

    // Tüm buffer'ları kuyruğa koy
    for (size_t i = 0; i < req.count; ++i)
    {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
        {
            LOG_ERROR("Failed to queue buffer " << i << " for device: " << device);
            for (size_t j = 0; j <= i; ++j)
            {
                if (buffers[j].start && buffers[j].start != MAP_FAILED)
                {
                    munmap(buffers[j].start, buffers[j].length);
                }
            }
            close(fd);
            boolResult = false;
            return CODE_DVI_MANAGER_BUFFER_QUEUE_FAILED;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0)
    {
        LOG_ERROR("Failed to start streaming for device: " << device);
        for (auto &b : buffers)
        {
            if (b.start && b.start != MAP_FAILED)
                munmap(b.start, b.length);
        }
        close(fd);
        boolResult = false;
        return CODE_DVI_MANAGER_START_STREAMING_FAILED;
    }

    checkCardResolution(channelId);
    boolResult = true;
    return CODE_SUCCESS;
}
uint8_t DviManager::cleanup(int fd, std::vector<Buffer> &buffers)
{
    try
    {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (fd >= 0)
        {
            ioctl(fd, VIDIOC_STREAMOFF, &type);
        }

        for (auto &b : buffers)
        {
            if (b.start && b.start != MAP_FAILED)
            {
                munmap(b.start, b.length);
            }
        }

        if (fd >= 0)
        {
            close(fd);
        }

        LOG_INFO("Device cleanup successful.");
        return CODE_SUCCESS;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Cleanup failed: " << e.what());
        return CODE_DVI_MANAGER_CLEANUP_FAILED;
    }
}

uint8_t DviManager::printStatistics()
{
    try
    {
        std::lock_guard<std::mutex> lock(mtx);
        std::cout << "\nDVI STATISTICS\n";
        std::cout << "-----------------------------\n";
        std::cout << "  Channel 1 - Error frames: " << errorFrameCountCh1
                  << " / Total frames: " << frameCountCh1 << std::endl;
        std::cout << "  Channel 2 - Error frames: " << errorFrameCountCh2
                  << " / Total frames: " << frameCountCh2 << std::endl;
        std::cout << "--------------------------------\n";
        return CODE_SUCCESS;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("DVI Print Statistics Error: " << e.what());
        return CODE_DVI_MANAGER_PRINT_STATISTICS_FAILED;
    }
}

uint8_t DviManager::isAnyChannelRunning(bool &result)
{
    try
    {
        result = runningCh1 || runningCh2;
        return CODE_SUCCESS;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("DVI Check Running Error: " << e.what());
        result = false;
        return CODE_DVI_MANAGER_CHECK_RUNNING_FAILED;
    }
}

uint8_t DviManager::isChannelRunning(int channel, bool &result)
{
    try
    {
        switch (channel)
        {
        case 0:
            result = runningCh1;
            break;
        case 1:
            result = runningCh2;
            break;
        case 2:
            result = runningCh1 || runningCh2;
            break;
        default:
            result = false;
            return CODE_DVI_MANAGER_INVALID_CHANNEL;
        }
        return CODE_SUCCESS;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("DVI Check Channel Running Error: " << e.what());
        result = false;
        return CODE_DVI_MANAGER_CHECK_CHANNEL_RUNNING_FAILED;
    }
}

uint8_t DviManager::getCurrentTimestamp(std::string &time)
{
    try
    {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d_%H-%M-%S");
        time = ss.str();
        return CODE_SUCCESS;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("DVI Get Current Timestamp Error: " << e.what());
        return CODE_DVI_MANAGER_GET_CURRENT_TIMESTAMP_FAILED;
    }
}

uint8_t DviManager::drawStatistics(cv::Mat &frame, int channel)
{
    try
    {
        double fps = (channel == 0) ? currentFpsCh1 : currentFpsCh2;
        int frames = (channel == 0) ? frameCountCh1 : frameCountCh2;
        int errors = (channel == 0) ? errorFrameCountCh1 : errorFrameCountCh2;

        cv::putText(frame, "FPS: " + std::to_string(static_cast<int>(fps)),
                    cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);
        cv::putText(frame, "Frames: " + std::to_string(frames),
                    cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);
        cv::putText(frame, "Errors: " + std::to_string(errors),
                    cv::Point(10, 90), cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 255), 2);
        return CODE_SUCCESS;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("DVI Draw Statistics Error: " << e.what());
        return CODE_DVI_MANAGER_DRAW_STATISTICS_FAILED;
    }
}

uint8_t DviManager::resetStatistics(int channel)
{
    try
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (channel == 0)
        {
            frameCountCh1 = 0;
            errorFrameCountCh1 = 0;
            currentFpsCh1 = 0.0;
            lastFrameTimeCh1 = std::chrono::steady_clock::now();
            startTimeCh1 = lastFrameTimeCh1;
        }
        else if (channel == 1)
        {
            frameCountCh2 = 0;
            errorFrameCountCh2 = 0;
            currentFpsCh2 = 0.0;
            lastFrameTimeCh2 = std::chrono::steady_clock::now();
            startTimeCh2 = lastFrameTimeCh2;
        }
        return CODE_SUCCESS;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("DVI Reset Statistics Error: " << e.what());
        return CODE_DVI_MANAGER_RESET_STATISTICS_FAILED;
    }
}

uint8_t DviManager::checkCardResolution(int channel)
{
    try
    {
        auto [actualW, actualH] = getInputResolution(channel);
        check_resolutionDvi(actualW, actualH, WIDTH, HEIGHT, channel);
        return CODE_SUCCESS;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("DVI Resolution Check Error: " << e.what());
        return CODE_DVI_MANAGER_RESOLUTION_CHECK_FAILED;
    }
}

uint8_t DviManager::getDeviceTemperature(int channelId, Channel &channel)
{
    try
    {
        std::string cmd = "mwcap-info --info-device 0:" + std::to_string(channelId);
        std::array<char, 256> buffer;
        temperature = -1.0f;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
        if (!pipe)
        {
            LOG_ERROR("Failed to run mwcap-info command for channel " << channelId);
            return CODE_RUN_MWCAP_INFO_FAILED;
        }
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
        {
            std::string line(buffer.data());
            if (line.find("Chipset temperature") != std::string::npos)
            {
                std::smatch match;
                std::regex tempRegex(R"((\d+(\.\d+)?))");
                if (std::regex_search(line, match, tempRegex))
                {
                    temperature = std::stof(match[1]);
                    channel.temperature.store(temperature);
                    break;
                }
            }
        }
        return CODE_SUCCESS;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("DVI Get Temperature Error: " << e.what());
        return CODE_DVI_MANAGER_GET_DEVICE_TEMPERATURE_FAILED;
    }
}

uint8_t DviManager::isSignalPresent(int channelId, bool &signalPresent)
{
    std::string cmd = "mwcap-info --info-all /dev/video" + std::to_string(channelId);
    std::array<char, 256> buffer;
    signalPresent = false;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe)
    {
        LOG_ERROR("Failed to run mwcap-info for channel " << channelId);
        return CODE_RUN_MWCAP_INFO_FAILED;
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
    {
        std::string line(buffer.data());
        if (line.find("Signal") != std::string::npos &&
            (line.find("Valid") != std::string::npos || line.find("Locked") != std::string::npos))
        {
            signalPresent = true;
            return CODE_SUCCESS;
        }
        if (line.find("Signal") != std::string::npos && line.find("None") != std::string::npos)
        {
            return CODE_SIGNAL_PRESENT_NONE;
        }
    }
    return CODE_SIGNAL_PRESENT_FAILED;
}

uint8_t DviManager::isErrorFrame(float previous_score, float current_score, cv::Mat &current_frame, std::string error_path, int &frame_counter, int &error_frame_counter)
{
    if (current_score < 0.96f && (current_score < (previous_score - 0.02f) || current_score > (previous_score + 0.02f)))
    {
        std::string filename = error_path + "/error_" + std::to_string(frame_counter) + "_err" + std::to_string(error_frame_counter) + ".png";
        cv::imwrite(filename, current_frame);
        error_frame_counter++;
    }

    return CODE_SUCCESS;
}