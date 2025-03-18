#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <functional>
#include <thread>
#include <windows.h>

uint32_t fntohl(uint32_t nlong) {
    return ((nlong & 0xFF000000) >> 24) |
           ((nlong & 0x00FF0000) >> 8)  |
           ((nlong & 0x0000FF00) << 8)  |
           ((nlong & 0x000000FF) << 24);
}

uint16_t fntohs(uint16_t nshort) {
    return ((nshort & 0xFF00) >> 8) |
           ((nshort & 0x00FF) << 8);
}

// Track data structure without class methods
struct TrackData {
    std::vector<uint8_t> data;
    std::vector<uint8_t> long_msg;
    int tick = 0;
    size_t offset = 0, length = 0;
    uint32_t message = 0, temp = 0;
    size_t long_msg_len = 0;
};

// Function declarations
int decode_variable_length(TrackData* track);
void update_tick(TrackData* track);
void update_command(TrackData* track);
void update_message(TrackData* track);
void process_meta_event(TrackData* track, double* multiplier, uint64_t* bpm, uint16_t time_div);

#if defined(_WIN32) || defined(_WIN64)
const auto NtQuerySystemTime = reinterpret_cast<void (*)(PLARGE_INTEGER)>(
    GetProcAddress(GetModuleHandle("ntdll.dll"), "NtQuerySystemTime"));

const auto NtDelayExecution = reinterpret_cast<void (*)(BOOL, PLARGE_INTEGER)>(
    GetProcAddress(GetModuleHandle("ntdll.dll"), "NtDelayExecution"));

void delayExecution100Ns(int64_t delayIn100Ns)
{
    LARGE_INTEGER delay;
    delay.QuadPart = -delayIn100Ns; // Negative for relative time
    NtDelayExecution(FALSE, &delay);
}

uint64_t get100NanosecondsSinceEpoch()
{
    LARGE_INTEGER ticker;
    NtQuerySystemTime(&ticker);
    return ticker.QuadPart;
}

#else
#include <time.h>
#include <sys/time.h>

uint64_t get100NanosecondsSinceEpoch()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    return ((uint64_t)ts.tv_sec * 10000000ULL) +
           ((uint64_t)ts.tv_nsec / 100ULL);
}

void delayExecution100Ns(int64_t delayIn100Ns)
{
    timespec req{};
    req.tv_sec = delayIn100Ns / 10000000;
    req.tv_nsec = (delayIn100Ns % 10000000) * 100;
    nanosleep(&req, nullptr);
}
#endif

// Implementation of track functions
int decode_variable_length(TrackData* track) {
    int result = 0;
    uint8_t byte;
    if (track->offset >= track->data.size()) return 0; // Avoid out-of-bounds
    do {
        byte = track->data[track->offset++];
        result = (result << 7) | (byte & 0x7F);
    } while (byte & 0x80 && track->offset < track->data.size()); // Prevent overflow
    return result;
}

void update_tick(TrackData* track)
{
    track->tick += decode_variable_length(track);
}

void update_command(TrackData* track)
{
    if (track->data.empty())
        return;
    const uint8_t temp = track->data[track->offset];
    if (temp >= 0x80)
    {
        track->offset++;
        track->message = temp;
    }
}

void update_message(TrackData* track) {
    if (track->data.empty()) return;

    const uint8_t msg_type = track->message & 0xFF;

    if (msg_type < 0xC0) {
        track->temp = track->data[track->offset] << 8 | track->data[track->offset + 1] << 16;
        track->offset += 2;
    } else if (msg_type < 0xE0) {
        track->temp = track->data[track->offset] << 8;
        track->offset += 1;
    } else if (msg_type < 0xF0) {
        track->temp = track->data[track->offset] << 8 | track->data[track->offset + 1] << 16;
        track->offset += 2;
    } else if (msg_type == 0xFF || msg_type == 0xF0) {
        track->temp = (msg_type == 0xFF) ? track->data[track->offset] << 8 : 0;
        track->offset += 1;
        track->long_msg_len = decode_variable_length(track);

        // Avoid frequent reallocations
        if (track->long_msg.size() < track->long_msg_len) {
            track->long_msg.resize(track->long_msg_len);
        }
        std::memcpy(track->long_msg.data(), &track->data[track->offset], track->long_msg_len);
        track->offset += track->long_msg_len;
    }

    track->message |= track->temp;
}

void process_meta_event(TrackData* track, double* multiplier, uint64_t* bpm, uint16_t time_div)
{
    const uint8_t meta_type = track->message >> 8 & 0xFF;
    if (meta_type == 0x51) { // Tempo change
        *bpm = (track->long_msg[0] << 16) | (track->long_msg[1] << 8) | track->long_msg[2];
        *multiplier = static_cast<double>(*bpm * 10) / static_cast<double>(time_div);
        *multiplier = std::max(*multiplier, 1.0); // Ensure minimum multiplier of 1
    }
    else if (meta_type == 0x2F) { // End of track
        track->data.clear();
    }
}

void log_notes_per_second(const bool* is_playing, uint64_t* note_on_count)
{
    while (*is_playing)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "Notes per second: " << *note_on_count << std::endl;
        *note_on_count = 0;
    }
}

// Function pointer type for SendDirectData
typedef void (*SendDirectDataFunc)(uint32_t);

void play_midi(std::vector<TrackData>* tracks, uint16_t time_div, SendDirectDataFunc SendDirectData)
{
    uint64_t tick = 0;
    double multiplier = 0;
    uint64_t bpm = 500000; // Default tempo: 120 BPM
    uint64_t delta_tick = 0;
    uint64_t last_time = 0;
    // ReSharper disable once CppTooWideScope
    constexpr uint64_t max_drift = 100000;
    int64_t delta = 0;
    uint64_t old = 0;
    uint64_t temp = 0;

    uint64_t note_on_count = 0;

    bool is_playing = false;

    const auto now = get100NanosecondsSinceEpoch();

    last_time = now;

    is_playing = true;

    std::thread note_logger(log_notes_per_second, &is_playing, &note_on_count);

    while (!tracks->empty())
    {
        std::vector<TrackData*> active_tracks;

        for (auto& track : *tracks)
        {
            if (!track.data.empty() && track.tick <= tick)
            {
                while (!track.data.empty() && track.tick <= tick)
                {
                    update_command(&track);
                    update_message(&track);

                    uint8_t msg_type = track.message & 0xFF;
                    if (msg_type < 0xF0)
                    {
                        SendDirectData(track.message);

                        // Note on
                        if (msg_type >= 0x90 && msg_type <= 0x9F)
                        {
                            note_on_count++;
                        }
                    }
                    else if (msg_type == 0xFF)
                    {
                        process_meta_event(&track, &multiplier, &bpm, time_div);
                    }
                    else if (msg_type == 0xF0)
                    {
                        std::cout << "TODO: Handle SysEx" << std::endl;
                    }

                    if (!track.data.empty())
                    {
                        update_tick(&track);
                    }
                }
                active_tracks.push_back(&track);
            }
            else if (!track.data.empty())
            {
                active_tracks.push_back(&track);
            }
        }

        if (active_tracks.empty())
        {
            break;
        }

        delta_tick = UINT64_MAX;
        for (const auto& track : active_tracks)
        {
            if (!track->data.empty())
            {
                delta_tick = std::min(delta_tick, track->tick - tick);
            }
        }

        tick += delta_tick;

        const auto now = get100NanosecondsSinceEpoch();
        temp = now - last_time;
        last_time = now;
        temp -= old;
        old = delta_tick * multiplier;
        delta += temp;

        temp = (delta > 0) ? (old - delta) : old;

        if (temp <= 0)
        {
            delta = std::min(delta, static_cast<int64_t>(max_drift));
        }
        else
        {
            delayExecution100Ns(temp);
        }
    }

    is_playing = false;
    note_logger.join();
}

std::vector<TrackData> load_midi_file(const std::string& filename, uint16_t* time_div)
{
    std::vector<TrackData> tracks;
    std::ifstream file(filename, std::ios::binary);

    if (!file.is_open())
    {
        throw std::runtime_error("Could not open file");
    }

    const auto start_time = std::chrono::high_resolution_clock::now();

    char header[4];
    file.read(header, 4);
    if (std::strncmp(header, "MThd", 4) != 0)
    {
        throw std::runtime_error("Not a MIDI file");
    }

    uint32_t header_length;
    file.read(reinterpret_cast<char*>(&header_length), 4);
    header_length = fntohl(header_length);
    if (header_length != 6)
    {
        throw std::runtime_error("Invalid header length");
    }

    uint16_t format;
    file.read(reinterpret_cast<char*>(&format), 2);
    format = fntohs(format);

    uint16_t track_count;
    file.read(reinterpret_cast<char*>(&track_count), 2);
    track_count = fntohs(track_count);

    file.read(reinterpret_cast<char*>(time_div), 2);
    *time_div = fntohs(*time_div);

    if (*time_div >= 0x8000)
    {
        throw std::runtime_error("SMPTE timing not supported");
    }

    std::cout << track_count << " tracks" << std::endl;

    for (int i = 0; i < track_count; ++i)
    {
        file.read(header, 4);
        if (std::strncmp(header, "MTrk", 4) != 0)
        {
            continue;
        }

        uint32_t length;
        file.read(reinterpret_cast<char*>(&length), 4);
        length = fntohl(length);

        TrackData track;
        track.data.resize(length);
        track.long_msg.reserve(256); // Reserve reasonable space for long messages
        track.length = length;

        file.read(reinterpret_cast<char*>(track.data.data()), length);
        tracks.push_back(track);

        update_tick(&tracks.back());
    }

    const auto end_time = std::chrono::high_resolution_clock::now();
    const auto duration_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    const auto duration_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::cout << "Parsed in " << duration_milliseconds << "ms (" << duration_microseconds << "\xE6s). " << std::endl;

    return tracks;
}

HMODULE initialize_midi(SendDirectDataFunc* SendDirectData)
{
    HMODULE midi = LoadLibrary(R"(C:\WINDOWS\system32\OmniMIDI.dll)");

    if (!midi)
    {
        throw std::runtime_error("Failed to load OmniMIDI.dll");
    }

    const auto IsKDMAPIAvailable = reinterpret_cast<BOOL(*)()>(
        GetProcAddress(midi, "IsKDMAPIAvailable"));
    const auto InitializeKDMAPIStream = reinterpret_cast<BOOL(*)()>(
        GetProcAddress(midi, "InitializeKDMAPIStream"));

    if (!IsKDMAPIAvailable || !InitializeKDMAPIStream ||
        !IsKDMAPIAvailable() || !InitializeKDMAPIStream())
    {
        throw std::runtime_error("MIDI initialization failed");
    }

    *SendDirectData = reinterpret_cast<void(*)(uint32_t)>(
        GetProcAddress(midi, "SendDirectData"));

    if (!*SendDirectData)
    {
        throw std::runtime_error("Failed to load required functions.");
    }

    return midi;
}

int main(const int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <midi_file>" << std::endl;
        return 1;
    }

    try
    {
        const auto start_time = std::chrono::high_resolution_clock::now();

        // Initialize MIDI
        SendDirectDataFunc SendDirectData;
        HMODULE midi = initialize_midi(&SendDirectData);

        // Load MIDI file
        uint16_t time_div = 0;
        std::vector<TrackData> tracks = load_midi_file(argv[1], &time_div);

        const auto end_time = std::chrono::high_resolution_clock::now();
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        std::cout << "MIDI initialization took " << duration << "ms.\n";

        std::cout << "\n\n\nPlaying midi file: " << argv[1] << std::endl;

        play_midi(&tracks, time_div, SendDirectData);

        FreeLibrary(midi);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}