// dgmod-receiver: plays the native DSD stream of dgmod-bridge (network output) on a local DAC through ALSA.

#include "AlsaSink.h"
#include "Receiver.h"

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

dgmod::receiver::Receiver* g_receiver = nullptr;
int g_priority = 70;

void OnSignal(int) {
    if (g_receiver) g_receiver->Stop();
}

void RealTimeAudioThread() {
    sched_param p{};
    p.sched_priority = g_priority;
    if (g_priority > 0 && pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) != 0)
        std::fprintf(stderr, "dgmod-receiver: no real-time priority for the audio thread (run as root, or set "
                             "LimitRTPRIO in the service); continuing without\n");
}

void Usage() {
    std::printf(
        "Usage: dgmod-receiver [options]\n"
        "Plays the native DSD stream of dgmod (output \"DSD (network)\") on a DAC through ALSA.\n\n"
        "  --device NAME     ALSA device, e.g. hw:CARD=K13,DEV=0 (default: the first card with native DSD)\n"
        "  --port N          TCP port to listen on (default %u)\n"
        "  --period-ms X     ALSA period, the report interval (default 2)\n"
        "  --buffer-ms X     ALSA buffer (default 8)\n"
        "  --hold S          keep the DAC in DSD silence for S seconds after the sender left (default 10)\n"
        "  --priority N      SCHED_FIFO priority of the audio thread, 0 = none (default 70)\n"
        "  --list            show the sound cards and their native DSD rates, then exit\n"
        "  --verbose         log every queue under-run\n",
        static_cast<unsigned>(dgmod::net::kDefaultPort));
}

}  // namespace

int main(int argc, char** argv) {
    dgmod::receiver::Options opt;
    std::string device;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "dgmod-receiver: %s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--device") device = value();
        else if (a == "--port") opt.port = static_cast<uint16_t>(std::atoi(value()));
        else if (a == "--period-ms") opt.periodMs = std::atof(value());
        else if (a == "--buffer-ms") opt.bufferMs = std::atof(value());
        else if (a == "--hold") opt.holdSec = std::atof(value());
        else if (a == "--priority") g_priority = std::atoi(value());
        else if (a == "--verbose") opt.verbose = true;
        else if (a == "--list") {
            const auto lines = dgmod::receiver::AlsaSink::Describe();
            if (lines.empty()) std::printf("no sound cards\n");
            for (const auto& l : lines) std::printf("%s\n", l.c_str());
            return 0;
        } else {
            Usage();
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    if (opt.periodMs < 0.5 || opt.periodMs > 50 || opt.bufferMs < 2 * opt.periodMs || opt.bufferMs > 500) {
        std::fprintf(stderr, "dgmod-receiver: need 0.5 <= period-ms <= 50 and 2 x period-ms <= buffer-ms <= 500\n");
        return 2;
    }
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        std::fprintf(stderr, "dgmod-receiver: memory not locked (LimitMEMLOCK); page faults may cause dropouts\n");
    opt.onAudioThread = &RealTimeAudioThread;

    dgmod::receiver::AlsaSink sink(device);
    dgmod::receiver::Receiver receiver(sink, opt);
    std::string error;
    if (!receiver.Listen(error)) {
        std::fprintf(stderr, "dgmod-receiver: %s\n", error.c_str());
        return 1;
    }
    g_receiver = &receiver;
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    receiver.Run();
    g_receiver = nullptr;
    return 0;
}
