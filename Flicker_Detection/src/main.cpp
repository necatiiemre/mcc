#include "FlickerDetectionVelocity.h"
#include "FlickerDetectionDvi.h"
#include "DviManager.h"
#include "DriverManager.h"
#include <thread>
#include <iostream>
#include <string>
#include <optional>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <cstdlib>
#include <atomic>
#include "Globals.h"
#include "ErrorUtils.h"
#include <opencv2/core/ocl.hpp>
#include <opencv2/opencv.hpp>

std::atomic<bool> stopRequested{false};

/* Velocity Flicker Detection Thread */
void flickerDetectionTask(int card1Input, int channel1Input,
                          std::optional<Card> card2, std::optional<Channel> channel2, bool loopback_test)
{
    try
    {
        if (card2.has_value() && channel2.has_value())
        {
            startFlickerDetection(
                static_cast<Card>(card1Input),
                static_cast<Channel>(channel1Input),
                card2,
                channel2,
                loopback_test);
        }
        else
        {
            startFlickerDetection(
                static_cast<Card>(card1Input),
                static_cast<Channel>(channel1Input),
                std::nullopt,
                std::nullopt,
                loopback_test);
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Velocity Flicker Detection Error: " << e.what() << std::endl;
    }
}

/* DVI Flicker Detection Thread */
void flickerDetectionDviTask(int channel)
{
    rc = startFlickerDetectionDvi(channel);
    checkReturnCode(rc, "DVI Flicker Detection failed.");
}

/* Command Interface */
void processCommands()
{
    std::string command;
    while (true)
    {
        std::cout << "\n🔹🔹🔹🔹 COMMANDS 🔹🔹🔹🔹\n";
        std::cout << "1. stats          - Show statistics for all\n";
        std::cout << "2. stop all       - Stop all flicker detection\n";
        std::cout << "3. stop card      - Stop specific Velocity card/channel\n";
        std::cout << "4. restart card   - Restart specific Velocity card/channel\n";
        std::cout << "5. stop dvi       - Stop DVI detection\n";
        std::cout << "6. restart dvi    - Restart DVI detection\n";
        std::cout << "7. exit           - Exit program\n";
        std::cout << "Command: ";

        std::getline(std::cin, command);

        // Trim spaces
        command.erase(0, command.find_first_not_of(" \t\n\r"));
        command.erase(command.find_last_not_of(" \t\n\r") + 1);

        if (command == "stats" || command == "1")
        {
            rc = driver_manager.printStatistics();
            checkReturnCode(rc, "Print Statistics Failed.");
            rc = dvi_manager.printStatistics();
            checkReturnCode(rc, "DVI Manager print statistics failed.");
        }
        else if (command == "stop all" || command == "2")
        {
            std::cout << "Stopping all systems...\n";
            stopRequested = true;

            rc = driver_manager.stopFlickerDetection();
            checkReturnCode(rc, "Stop Flicker Detection Failed.");
            rc = dvi_manager.stop(2);
            checkReturnCode(rc, "DVI Manager stop failed.");
            break;
        }
        else if (command == "stop card" || command == "3")
        {
            std::cout << "Which Card (0: CARD_1, 1: CARD_2, 2: CARD_BOTH): ";
            std::getline(std::cin, command);
            Card card = static_cast<Card>(std::stoi(command));

            std::cout << "Which Channel (0: CH_1, 1: CH_2, 2: CH_BOTH): ";
            std::getline(std::cin, command);
            Channel channel = static_cast<Channel>(std::stoi(command));

            driver_manager.stopCard(card, channel);
        }
        else if (command == "restart card" || command == "4")
        {
            std::cout << "Which Card (0: CARD_1, 1: CARD_2, 2: CARD_BOTH): ";
            std::getline(std::cin, command);
            Card card = static_cast<Card>(std::stoi(command));

            std::cout << "Which Channel (0: CH_1, 1: CH_2, 2: CH_BOTH): ";
            std::getline(std::cin, command);
            Channel channel = static_cast<Channel>(std::stoi(command));

            driver_manager.restartCard(card, channel);
        }
        else if (command == "stop dvi" || command == "5")
        {
            std::cout << "Which DVI Channel to stop (0/1/2): ";
            std::getline(std::cin, command);
            int ch = std::stoi(command);
            cout << "select channel " << ch <<endl;
            rc = dvi_manager.stop(ch);
            checkReturnCode(rc, "DVI Manager stop failed.");
        }
        else if (command == "restart dvi" || command == "6")
        {
            std::cout << "Which DVI Channel to start (0/1/2): ";
            std::getline(std::cin, command);
            int ch = std::stoi(command);
            rc = dvi_manager.start(ch);
            checkReturnCode(rc, "DVI Manager start failed.");
        }
        else if (command == "exit" || command == "7")
        {
            stopRequested = true;
            break;
        }
        else
        {
            std::cout << "❌ Invalid Command.\n";
        }
    }
}

int main()
{
    std::cout << cv::getBuildInformation() << std::endl;

    cv::setNumThreads(1);
    cv::ocl::setUseOpenCL(false);

    std::string modeSelection;
    std::cout << "🟢 Choose Flicker Detection Mode:\n";
    std::cout << "1 - Velocity\n";
    std::cout << "2 - DVI\n";
    std::cout << "3 - Both\n";
    std::cout << "Selection: ";
    std::getline(std::cin, modeSelection);

    std::thread velocity_thread;
    std::thread dvi_thread;

    if (modeSelection == "1" || modeSelection == "velocity")
    {
        int card1Input, channel1Input;
        std::optional<Card> card2;
        std::optional<Channel> channel2;

        std::cout << "Card1 (0: CARD_1, 1: CARD_2, 2: CARD_BOTH): ";
        std::cin >> card1Input;
        std::cout << "Channel1 (0: CH_1, 1: CH_2, 2: CH_BOTH): ";
        std::cin >> channel1Input;
        std::cin.ignore();

        if (card1Input != static_cast<int>(Card::CARD_BOTH))
        {
            std::string secondCardChoice;
            std::cout << "Use second card? (y/n): ";
            std::getline(std::cin, secondCardChoice);

            if (secondCardChoice == "y" || secondCardChoice == "Y")
            {
                int card2Input, channel2Input;
                std::cout << "Card2 (0: CARD_1, 1: CARD_2, 2: CARD_BOTH): ";
                std::cin >> card2Input;
                std::cout << "Channel2 (0: CH_1, 1: CH_2, 2: CH_BOTH): ";
                std::cin >> channel2Input;
                std::cin.ignore();

                card2 = static_cast<Card>(card2Input);
                channel2 = static_cast<Channel>(channel2Input);
            }
        }

        std::cout << "Do you want to loopback test ? (y/n): ";
        std::string loopbackChoice;
        std::getline(std::cin, loopbackChoice);
        loopbackTestMode = (loopbackChoice == "y" || loopbackChoice == "Y");

        velocity_thread = std::thread(flickerDetectionTask, card1Input, channel1Input, card2, channel2, loopbackTestMode);
    }
    else if (modeSelection == "2" || modeSelection == "dvi")
    {
        int dviChannel;
        std::cout << "DVI Channel (0: CH1 /dev/video0, 1: CH2 /dev/video1, 2: both): ";
        std::cin >> dviChannel;
        std::cin.ignore();

        dvi_thread = std::thread(flickerDetectionDviTask, dviChannel);
    }
    else if (modeSelection == "3" || modeSelection == "both")
    {
        int card1Input, channel1Input, dviChannel;
        std::optional<Card> card2;
        std::optional<Channel> channel2;

        std::cout << "\nVelocity Configuration:\n";
        std::cout << "Card1 (0: CARD_1, 1: CARD_2, 2: CARD_BOTH): ";
        std::cin >> card1Input;
        std::cout << "Channel1 (0: CH_1, 1: CH_2, 2: CH_BOTH): ";
        std::cin >> channel1Input;
        std::cin.ignore();

        if (card1Input != static_cast<int>(Card::CARD_BOTH))
        {
            std::string secondCardChoice;
            std::cout << "Use second card? (y/n): ";
            std::getline(std::cin, secondCardChoice);

            if (secondCardChoice == "y" || secondCardChoice == "Y")
            {
                int card2Input, channel2Input;
                std::cout << "Card2 (0: CARD_1, 1: CARD_2, 2: CARD_BOTH): ";
                std::cin >> card2Input;
                std::cout << "Channel2 (0: CH_1, 1: CH_2, 2: CH_BOTH): ";
                std::cin >> channel2Input;
                std::cin.ignore();

                card2 = static_cast<Card>(card2Input);
                channel2 = static_cast<Channel>(channel2Input);
            }
        }

        std::cout << "Do you want to loopback test ? (y/n): ";
        std::string loopbackChoice;
        std::getline(std::cin, loopbackChoice);
        loopbackTestMode = (loopbackChoice == "y" || loopbackChoice == "Y");

        std::cout << "\nDVI Configuration:\n";
        std::cout << "DVI Channel (0: CH1 /dev/video0, 1: CH2 /dev/video1, 2: both): ";
        std::cin >> dviChannel;
        std::cin.ignore();

        velocity_thread = std::thread(flickerDetectionTask, card1Input, channel1Input, card2, channel2, loopbackTestMode);
        dvi_thread = std::thread(flickerDetectionDviTask, dviChannel);
    }
    else
    {
        std::cerr << "❌ Invalid mode selection.\n";
        return 1;
    }

    // Start commands in separate thread
    std::thread command_thread(processCommands);

    // Wait for all threads
    if (velocity_thread.joinable())
        velocity_thread.join();
    if (dvi_thread.joinable())
        dvi_thread.join();
    if (command_thread.joinable())
        command_thread.join();

    std::cout << "✅ Program terminated successfully.\n";
    return 0;
}
