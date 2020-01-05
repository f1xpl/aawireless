#include <QCoreApplication>
#include <QtDBus/QDBusConnection>
#include <thread>
#include <libusb.h>
#include "aawireless/bluetooth/BluetoothService.h"
#include <boost/asio/io_service.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <aawireless/log/Log.h>
#include <f1x/aasdk/TCP/TCPWrapper.hpp>
#include <f1x/aasdk/USB/USBWrapper.hpp>
#include <f1x/aasdk/USB/USBHub.hpp>
#include <f1x/aasdk/USB/AccessoryModeQueryFactory.hpp>
#include <f1x/aasdk/USB/AccessoryModeQueryChainFactory.hpp>
#include <f1x/aasdk/USB/ConnectedAccessoriesEnumerator.hpp>
#include <aawireless/App.h>
#include <aawireless/connection/ConnectionFactory.h>
#include <aawireless/configuration/Configuration.h>
#include <aawireless/database/Database.h>

using ThreadPool = std::vector<std::thread>;

void startUSBWorkers(boost::asio::io_service &ioService, libusb_context *usbContext, ThreadPool &threadPool) {
    auto usbWorker = [&ioService, usbContext]() {
        timeval libusbEventTimeout{180, 0};

        while (!ioService.stopped()) {
            libusb_handle_events_timeout_completed(usbContext, &libusbEventTimeout, nullptr);
        }
    };

    threadPool.emplace_back(usbWorker);
    threadPool.emplace_back(usbWorker);
    threadPool.emplace_back(usbWorker);
    threadPool.emplace_back(usbWorker);
}

void startIOServiceWorkers(boost::asio::io_service &ioService, ThreadPool &threadPool) {
    auto ioServiceWorker = [&ioService]() {
        ioService.run();
    };

    threadPool.emplace_back(ioServiceWorker);
    threadPool.emplace_back(ioServiceWorker);
    threadPool.emplace_back(ioServiceWorker);
    threadPool.emplace_back(ioServiceWorker);
}

int main(int argc, char *argv[]) {
    if (!QDBusConnection::systemBus().isConnected()) {
        AW_LOG(error) << "Cannot connect to the D-Bus session bus.";
        return 1;
    }

    libusb_context *usbContext;
    if (libusb_init(&usbContext) != 0) {
        AW_LOG(error) << "[OpenAuto] libusb init failed.";
        return 1;
    }

    boost::asio::io_service ioService;
    boost::asio::io_service::work work(ioService);
    std::vector<std::thread> threadPool;
    startUSBWorkers(ioService, usbContext, threadPool);
    startIOServiceWorkers(ioService, threadPool);

    QCoreApplication qApplication(argc, argv);

    aawireless::configuration::Configuration configuration("config.ini");
    aawireless::database::Database database("/var/lib/aawireless/db.ini");
    f1x::aasdk::tcp::TCPWrapper tcpWrapper;
    f1x::aasdk::usb::USBWrapper usbWrapper(usbContext);
    f1x::aasdk::usb::AccessoryModeQueryFactory queryFactory(usbWrapper, ioService);
    f1x::aasdk::usb::AccessoryModeQueryChainFactory queryChainFactory(usbWrapper, ioService, queryFactory);
    boost::asio::ip::tcp::acceptor acceptor(ioService,
                                            boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 5000));
    aawireless::bluetooth::BluetoothService bluetoothService(configuration, database);
    auto usbHub = std::make_shared<f1x::aasdk::usb::USBHub>(usbWrapper, ioService, queryChainFactory);
    aawireless::connection::ConnectionFactory connectionFactory(ioService, tcpWrapper, usbWrapper);

    auto app = std::make_shared<aawireless::App>(ioService,
                                                 usbHub,
                                                 acceptor,
                                                 bluetoothService,
                                                 connectionFactory,
                                                 configuration);
    app->start();

    auto result = qApplication.exec();

    app->stop();

    std::for_each(threadPool.begin(), threadPool.end(), std::bind(&std::thread::join, std::placeholders::_1));

    libusb_exit(usbContext);

    return result;
}
