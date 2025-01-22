#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <functional>
#include <thread>
#include <windows.h>

#ifndef ntohl
#define ntohl(x) ((uint32_t)((((x) & 0xFF) << 24) | (((x) & 0xFF00) << 8) | (((x) & 0xFF0000) >> 8) | (((x) >> 24) & 0xFF)))
#endif

#ifndef ntohs
#define ntohs(x) ((uint16_t)((((x) & 0xFF) << 8) | (((x) & 0xFF00) >> 8)))
#endif

class Track
{
public:
    std::vector<uint8_t> data;
    int tick = 0;
    size_t offset = 0;
    size_t length;
    uint32_t message = 0;
    uint32_t temp = 0;
    size_t long_msg_len = 0;
    std::vector<uint8_t> long_msg;

    explicit Track(const std::vector<uint8_t> &track_data) : data(track_data), length(track_data.size()) {}

    int decode_variable_length()
    {
        int result = 0;
        while (true)
        {
            const uint8_t byte = data[offset];
            result = (result << 7) + (byte & 0x7F);
            offset++;
            if (byte < 0x80)
                break;
        }
        return result;
    }

    void update_tick()
    {
        tick += decode_variable_length();
    }

    void update_command()
    {
        if (data.empty())
            return;
        const uint8_t temp = data[offset];
        if (temp >= 0x80)
        {
            offset++;
            message = temp;
        }
    }

    void update_message()
    {
        if (data.empty())
            return;
        const uint8_t msg_type = message & 0xFF;

        if (msg_type < 0xC0)
        {
            temp = data[offset] << 8;
            temp |= data[offset + 1] << 16;
            offset += 2;
        }
        else if (msg_type < 0xE0)
        {
            temp = data[offset] << 8;
            offset += 1;
        }
        else if (msg_type < 0xF0)
        {
            temp = data[offset] << 8;
            temp |= data[offset + 1] << 16;
            offset += 2;
        }
        else if (msg_type == 0xFF)
        {
            temp = data[offset] << 8;
            offset += 1;
            long_msg_len = decode_variable_length();
            long_msg.assign(data.begin() + offset, data.begin() + offset + long_msg_len);
            offset += long_msg_len;
        }
        else if (msg_type == 0xF0)
        {
            temp = 0;
            long_msg_len = decode_variable_length();
            long_msg.assign(data.begin() + offset, data.begin() + offset + long_msg_len);
            offset += long_msg_len;
        }

        message |= temp;
    }
};

class MIDIPlayer
{
private:
    uint64_t tick = 0;
    double multiplier = 0;
    uint64_t bpm = 500000; // Default tempo: 120 BPM
    uint64_t delta_tick = 0;
    std::vector<Track> tracks;
    uint16_t time_div = 0;
    uint64_t last_time = 0;
    uint64_t max_drift = 100000;
    int64_t delta = 0;
    uint64_t old = 0;
    uint64_t temp = 0;

    uint64_t note_on_count = 0;
    uint64_t last_note_count_time = 0;

    bool is_playing = false;

    // std::vector<uint32_t> messages;
    // std::vector<LARGE_INTEGER> sleep_vals;

    HMODULE midi{};

    std::function<void(PLARGE_INTEGER)> NtQuerySystemTime;
    std::function<void(BOOL, PLARGE_INTEGER)> NtDelayExecution;
    std::function<void(uint32_t)> SendDirectData;

public:
    explicit MIDIPlayer(const std::string &filename)
    {
        initialize_midi();
        load_file(filename);
    }

    void load_file(const std::string &filename)
    {
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
        file.read(reinterpret_cast<char *>(&header_length), 4);
        header_length = ntohl(header_length);
        if (header_length != 6)
        {
            throw std::runtime_error("Invalid header length");
        }

        uint16_t format;
        file.read(reinterpret_cast<char *>(&format), 2);
        format = ntohs(format);

        uint16_t track_count;
        file.read(reinterpret_cast<char *>(&track_count), 2);
        track_count = ntohs(track_count);

        file.read(reinterpret_cast<char *>(&time_div), 2);
        time_div = ntohs(time_div);

        if (time_div >= 0x8000)
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
            file.read(reinterpret_cast<char *>(&length), 4);
            length = ntohl(length);

            std::vector<uint8_t> track_data(length);
            file.read(reinterpret_cast<char *>(track_data.data()), length);
            tracks.emplace_back(track_data);
            tracks.back().update_tick();
        }

        const auto end_time = std::chrono::high_resolution_clock::now();
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        std::cout << "Parsed in " << duration << "ms." << std::endl;
    }

    void initialize_midi()
    {
        midi = LoadLibrary(R"(C:\WINDOWS\system32\OmniMIDI.dll)");
        // midi = LoadLibrary(R"(C:\Users\ar06\Documents\MidiPlayers\omv2\x64\OmniMIDI.dll)");

        if (!midi)
        {
            throw std::runtime_error("Failed to load OmniMIDI.dll");
        }

        const auto IsKDMAPIAvailable = reinterpret_cast<BOOL (*)()>(
            GetProcAddress(midi, "IsKDMAPIAvailable"));
        const auto InitializeKDMAPIStream = reinterpret_cast<BOOL (*)()>(
            GetProcAddress(midi, "InitializeKDMAPIStream"));

        if (!IsKDMAPIAvailable || !InitializeKDMAPIStream ||
            !IsKDMAPIAvailable() || !InitializeKDMAPIStream())
        {
            throw std::runtime_error("MIDI initialization failed");
        }

        NtQuerySystemTime = reinterpret_cast<void (*)(PLARGE_INTEGER)>(
            GetProcAddress(GetModuleHandle("ntdll.dll"), "NtQuerySystemTime"));
        NtDelayExecution = reinterpret_cast<void (*)(BOOL, PLARGE_INTEGER)>(
            GetProcAddress(GetModuleHandle("ntdll.dll"), "NtDelayExecution"));
        SendDirectData = reinterpret_cast<void (*)(uint32_t)>(
            GetProcAddress(midi, "SendDirectData"));

        if (!SendDirectData || !NtQuerySystemTime || !NtDelayExecution)
        {
            throw std::runtime_error("Failed to load required functions.");
        }
    }

    void log_notes_per_second()
    {
        while (is_playing)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            std::cout << "Notes per second: " << note_on_count << std::endl;

            note_on_count = 0;
        }
    }

    void play()
    {
        LARGE_INTEGER ticker;
        NtQuerySystemTime(&ticker);
        last_time = ticker.QuadPart;
        last_note_count_time = last_time;

        is_playing = true;

        std::thread note_logger(&MIDIPlayer::log_notes_per_second, this);

        while (!tracks.empty())
        {
            std::vector<Track *> active_tracks;

            for (auto &track : tracks)
            {
                if (!track.data.empty() && track.tick <= tick)
                {
                    while (!track.data.empty() && track.tick <= tick)
                    {
                        track.update_command();
                        track.update_message();

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
                            process_meta_event(track);
                        }
                        else if (msg_type == 0xF0)
                        {
                            std::cout << "TODO: Handle SysEx" << std::endl;
                        }

                        if (!track.data.empty())
                        {
                            track.update_tick();
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
            for (const auto &track : active_tracks)
            {
                if (!track->data.empty())
                {
                    delta_tick = std::min(delta_tick, track->tick - tick);
                }
            }

            tick += delta_tick;

            NtQuerySystemTime(&ticker);
            temp = ticker.QuadPart - last_time;
            last_time = ticker.QuadPart;
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
                LARGE_INTEGER sleep_val;
                sleep_val.QuadPart = -static_cast<LONGLONG>(temp);
                NtDelayExecution(FALSE, &sleep_val);
            }

            // std::cout << "delta_tick: " << delta_tick << std::endl;
        }

        is_playing = false;

        note_logger.join();
    }

    void process_meta_event(Track &track)
    {
        const uint8_t meta_type = track.message >> 8 & 0xFF;
        if (meta_type == 0x51)
        { // Tempo change
            bpm = (track.long_msg[0] << 16) | (track.long_msg[1] << 8) | track.long_msg[2];
            multiplier = static_cast<double>(bpm * 10) / static_cast<double>(time_div);
            multiplier = std::max(multiplier, 1.0); // Ensure minimum multiplier of 1
        }
        else if (meta_type == 0x2F)
        { // End of track
            track.data.clear();
        }
        else if (meta_type < 0x10)
        {
            // std::cout << "Meta" << std::hex << static_cast<int>(meta_type) << ": " << track.long_msg.size() << " bytes" << std::endl;
        }
    }
};

int main(const int argc, char *argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <midi_file>" << std::endl;
        return 1;
    }

    try
    {
        const auto start_time = std::chrono::high_resolution_clock::now();

        MIDIPlayer player(argv[1]);

        const auto end_time = std::chrono::high_resolution_clock::now();
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        std::cout << "MIDIPlayer initialization took " << duration << "ms.\n";

        // Sleep for 1 second because OmniMIDIv2 logs are absolutely massive.
        // std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "\n\n\nPlaying midi file: " << argv[1] << std::endl;

        player.play();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
