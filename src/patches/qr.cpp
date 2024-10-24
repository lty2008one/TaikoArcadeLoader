#include "constants.h"
#include "helpers.h"
#include "poll.h"
#include <ReadBarcode.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "stb_image.h"

extern GameVersion gameVersion;
extern Keybindings CARD_INSERT_1;
extern Keybindings CARD_INSERT_2;
extern Keybindings QR_DATA_READ;
extern Keybindings QR_IMAGE_READ;
extern char accessCode1[21];
extern char accessCode2[21];
extern std::vector<HMODULE> plugins;
extern bool emulateQr;

typedef void event ();
typedef void initQrEvent (GameVersion gameVersion);
typedef bool checkQrEvent ();
typedef int getQrEvent (int, unsigned char *);

namespace patches::Qr {

enum class State { Ready, CopyWait };
enum class Mode { Card, Data, Image, Plugin };
State gState = State::Ready;
Mode gMode   = Mode::Card;
HMODULE gPlugin;
std::string accessCode;

std::vector<HMODULE> qrPlugins;
bool qrPluginRegistered = false;

HOOK_DYNAMIC (char, __fastcall, QrInit, i64) { return 1; }
HOOK_DYNAMIC (char, __fastcall, QrClose, i64) { return 1; }
HOOK_DYNAMIC (char, __fastcall, QrRead, i64 a1) {
    *(DWORD *)(a1 + 40) = 1;
    *(DWORD *)(a1 + 16) = 1;
    *(BYTE *)(a1 + 112) = 0;
    return 1;
}
HOOK_DYNAMIC (i64, __fastcall, CallQrUnknown, i64) { return 1; }
HOOK_DYNAMIC (bool, __fastcall, Send1, i64 a1) {
    *(BYTE *)(a1 + 88) = 1;
    *(i64 *)(a1 + 32)  = *(i64 *)(a1 + 24);
    *(WORD *)(a1 + 89) = 0;
    return true;
}
HOOK_DYNAMIC (bool, __fastcall, Send2, i64 a1) {
    *(WORD *)(a1 + 88) = 0;
    *(BYTE *)(a1 + 90) = 0;
    return true;
}
HOOK_DYNAMIC (bool, __fastcall, Send3, i64, char) { return true; }
HOOK_DYNAMIC (bool, __fastcall, Send4, i64, const void *, i64) { return true; }
HOOK_DYNAMIC (i64, __fastcall, CopyData, i64, void *dest, int length) {
    if (gState == State::CopyWait) {
        std::cout << "Copy data, length: " << length << std::endl;

        auto configPath = std::filesystem::current_path () / "config.toml";
        std::unique_ptr<toml_table_t, void (*) (toml_table_t *)> config_ptr (openConfig (configPath), toml_free);

        if (gMode == Mode::Card) {
            memcpy (dest, accessCode.c_str (), accessCode.size () + 1);
            gState = State::Ready;
            return accessCode.size () + 1;
        } else if (gMode == Mode::Data) {
            std::string serial = "";
            u16 type           = 0;
            std::vector<i64> songNoes;

            if (config_ptr) {
                auto qr = openConfigSection (config_ptr.get (), "qr");
                if (qr) {
                    auto data = openConfigSection (qr, "data");
                    if (data) {
                        serial   = readConfigString (data, "serial", "");
                        type     = readConfigInt (data, "type", 0);
                        songNoes = readConfigIntArray (data, "song_no", songNoes);
                    }
                }
            }

            BYTE serial_length           = (BYTE)serial.size ();
            std::vector<BYTE> byteBuffer = {0x53, 0x31, 0x32, 0x00, 0x00, 0xFF, 0xFF, serial_length, 0x01, 0x00};

            for (char c : serial)
                byteBuffer.push_back ((BYTE)c);

            if (type == 5) {
                std::vector<BYTE> folderData = {0xFF, 0xFF};

                folderData.push_back (songNoes.size () * 2);

                folderData.push_back ((u8)(type & 0xFF));
                folderData.push_back ((u8)((type >> 8) & 0xFF));

                for (u16 songNo : songNoes) {
                    folderData.push_back ((u8)(songNo & 0xFF));
                    folderData.push_back ((u8)((songNo >> 8) & 0xFF));
                }

                for (auto c : folderData)
                    byteBuffer.push_back (c);
            }

            byteBuffer.push_back (0xEE);
            byteBuffer.push_back (0xFF);

            for (auto byteData : byteBuffer)
                std::cout << std::hex << std::uppercase << std::setfill ('0') << std::setw (2) << static_cast<int> (byteData) << " ";
            std::cout << std::endl;

            memcpy (dest, byteBuffer.data (), byteBuffer.size ());
            gState = State::Ready;
            return byteBuffer.size ();
        } else if (gMode == Mode::Image) {
            std::string imagePath = "";

            if (config_ptr) {
                auto qr = openConfigSection (config_ptr.get (), "qr");
                if (qr) imagePath = readConfigString (qr, "image_path", "");
            }

            std::u8string u8PathStr (imagePath.begin (), imagePath.end ());
            std::filesystem::path u8Path (u8PathStr);
            if (!std::filesystem::is_regular_file (u8Path)) {
                std::cerr << "Failed to open image: " << u8Path.string () << " (file not found)"
                          << "\n";
                gState = State::Ready;
                return 0;
            }

            int width, height, channels;
            std::unique_ptr<stbi_uc, void (*) (void *)> buffer (stbi_load (u8Path.string ().c_str (), &width, &height, &channels, 3),
                                                                stbi_image_free);
            if (!buffer) {
                std::cerr << "Failed to read image: " << u8Path << " (" << stbi_failure_reason () << ")"
                          << "\n";
                gState = State::Ready;
                return 0;
            }

            ZXing::ImageView image{buffer.get (), width, height, ZXing::ImageFormat::RGB};
            auto result = ReadBarcode (image);
            if (!result.isValid ()) {
                std::cerr << "Failed to read QR: " << imagePath << " (" << ToString (result.error ()) << ")"
                          << "\n";
                gState = State::Ready;
                return 0;
            }

            std::cout << "Valid" << std::endl;
            auto byteData = result.bytes ();
            std::cout << ZXing::ToHex (byteData) << std::endl;
            auto dataSize = byteData.size ();

            memcpy (dest, byteData.data (), dataSize);
            gState = State::Ready;
            return dataSize;
        } else if (gMode == Mode::Plugin) {
            FARPROC getEvent = GetProcAddress (gPlugin, "GetQr");
            if (getEvent) {
                unsigned char plugin_data[length];
                int buf_len = ((getQrEvent *)getEvent) (length, plugin_data);
                if (0 < buf_len && buf_len <= length) {
                    for (int i = 0; i < buf_len; i++)
                        std::cout << std::hex << std::uppercase << std::setfill ('0') << std::setw (2) << static_cast<int> (plugin_data[i]) << " ";
                    std::cout << std::endl;
                    memcpy (dest, plugin_data, buf_len);
                } else {
                    std::cerr << "QR discard! Length invalid: " << buf_len << ", valid range: 0~" << length << std::endl;
                }
                gState = State::Ready;
                return buf_len;
            } else {
                gState = State::Ready;
                return 0;
            }
        }
    } else if (qrPluginRegistered) {
        for (auto plugin : qrPlugins) {
            FARPROC usingQrEvent = GetProcAddress (plugin, "UsingQr");
            if (usingQrEvent) ((event *)usingQrEvent) ();
        }
    }
    return 0;
}

void
Update () {
    if (!emulateQr) return;
    if (gState == State::Ready) {
        if (IsButtonTapped (CARD_INSERT_1)) {
            if (gameVersion != GameVersion::CHN00) return;

            std::cout << "Insert" << std::endl;
            accessCode = "BNTTCNID";
            accessCode += accessCode1;
            gState = State::CopyWait;
            gMode  = Mode::Card;
        } else if (IsButtonTapped (CARD_INSERT_2)) {
            if (gameVersion != GameVersion::CHN00) return;

            std::cout << "Insert" << std::endl;
            accessCode = "BNTTCNID";
            accessCode += accessCode2;
            gState = State::CopyWait;
            gMode  = Mode::Card;
        } else if (IsButtonTapped (QR_DATA_READ)) {
            std::cout << "Insert" << std::endl;
            gState = State::CopyWait;
            gMode  = Mode::Data;
        } else if (IsButtonTapped (QR_IMAGE_READ)) {
            std::cout << "Insert" << std::endl;
            gState = State::CopyWait;
            gMode  = Mode::Image;
        } else if (qrPluginRegistered) {
            for (auto plugin : qrPlugins) {
                FARPROC checkEvent = GetProcAddress (plugin, "CheckQr");
                if (checkEvent && ((checkQrEvent *)checkEvent) ()) {
                    std::cout << "Insert" << std::endl;
                    gState  = State::CopyWait;
                    gMode   = Mode::Plugin;
                    gPlugin = plugin;
                    break;
                }
            }
        }
    }
}

void
Init () {
    if (!emulateQr) {
        std::cout << "[Init] QR emulation disabled" << std::endl;
        return;
    }

    for (auto plugin : plugins) {
        FARPROC initEvent = GetProcAddress (plugin, "InitQr");
        if (initEvent) ((initQrEvent *)initEvent) (gameVersion);

        FARPROC usingQrEvent = GetProcAddress (plugin, "UsingQr");
        if (usingQrEvent) qrPlugins.push_back (plugin);
    }
    if (qrPlugins.size () > 0) {
        std::cout << "QR plugin found!" << std::endl;
        qrPluginRegistered = true;
    }

    SetConsoleOutputCP (CP_UTF8);
    auto amHandle = (u64)GetModuleHandle ("AMFrameWork.dll");
    switch (gameVersion) {
    case GameVersion::JPN00: {
        INSTALL_HOOK_DYNAMIC (QrInit, (LPVOID)(amHandle + 0x1B3E0));
        INSTALL_HOOK_DYNAMIC (QrClose, (LPVOID)(amHandle + 0x1B5B0));
        INSTALL_HOOK_DYNAMIC (QrRead, (LPVOID)(amHandle + 0x1B600));
        INSTALL_HOOK_DYNAMIC (CallQrUnknown, (LPVOID)(amHandle + 0xFD40));
        INSTALL_HOOK_DYNAMIC (Send1, (LPVOID)(amHandle + 0x1BBB0));
        INSTALL_HOOK_DYNAMIC (Send2, (LPVOID)(amHandle + 0x1BBF0));
        INSTALL_HOOK_DYNAMIC (Send3, (LPVOID)(amHandle + 0x1BC60));
        // JPN00 has no Send4
        INSTALL_HOOK_DYNAMIC (CopyData, (LPVOID)(amHandle + 0x1BC30));
        break;
    }
    case GameVersion::JPN08: {
        INSTALL_HOOK_DYNAMIC (QrInit, (LPVOID)(amHandle + 0x1BA00));
        INSTALL_HOOK_DYNAMIC (QrClose, (LPVOID)(amHandle + 0x1BBD0));
        INSTALL_HOOK_DYNAMIC (QrRead, (LPVOID)(amHandle + 0x1BC20));
        INSTALL_HOOK_DYNAMIC (CallQrUnknown, (LPVOID)(amHandle + 0xFD40));
        INSTALL_HOOK_DYNAMIC (Send1, (LPVOID)(amHandle + 0x1C220));
        INSTALL_HOOK_DYNAMIC (Send2, (LPVOID)(amHandle + 0x1C260));
        INSTALL_HOOK_DYNAMIC (Send3, (LPVOID)(amHandle + 0x1C2D0));
        // JPN08 has no Send4
        INSTALL_HOOK_DYNAMIC (CopyData, (LPVOID)(amHandle + 0x1C2A0));
        break;
    }
    case GameVersion::JPN39: {
        INSTALL_HOOK_DYNAMIC (QrInit, (LPVOID)(amHandle + 0x1EDC0));
        INSTALL_HOOK_DYNAMIC (QrClose, (LPVOID)(amHandle + 0x1EF60));
        INSTALL_HOOK_DYNAMIC (QrRead, (LPVOID)(amHandle + 0x1EFB0));
        INSTALL_HOOK_DYNAMIC (CallQrUnknown, (LPVOID)(amHandle + 0x11A70));
        INSTALL_HOOK_DYNAMIC (Send1, (LPVOID)(amHandle + 0x1F5B0));
        INSTALL_HOOK_DYNAMIC (Send2, (LPVOID)(amHandle + 0x1F5F0));
        INSTALL_HOOK_DYNAMIC (Send3, (LPVOID)(amHandle + 0x1F660));
        INSTALL_HOOK_DYNAMIC (Send4, (LPVOID)(amHandle + 0x1F690));
        INSTALL_HOOK_DYNAMIC (CopyData, (LPVOID)(amHandle + 0x1F630));
        break;
    }
    case GameVersion::CHN00: {
        INSTALL_HOOK_DYNAMIC (QrInit, (LPVOID)(amHandle + 0x161B0));
        INSTALL_HOOK_DYNAMIC (QrClose, (LPVOID)(amHandle + 0x16350));
        INSTALL_HOOK_DYNAMIC (QrRead, (LPVOID)(amHandle + 0x163A0));
        INSTALL_HOOK_DYNAMIC (CallQrUnknown, (LPVOID)(amHandle + 0x8F60));
        INSTALL_HOOK_DYNAMIC (Send1, (LPVOID)(amHandle + 0x16940));
        INSTALL_HOOK_DYNAMIC (Send2, (LPVOID)(amHandle + 0x16990));
        INSTALL_HOOK_DYNAMIC (Send3, (LPVOID)(amHandle + 0x16A00));
        INSTALL_HOOK_DYNAMIC (Send4, (LPVOID)(amHandle + 0x16A30));
        INSTALL_HOOK_DYNAMIC (CopyData, (LPVOID)(amHandle + 0x169D0));
        break;
    }
    default: {
        break;
    }
    }
}
} // namespace patches::Qr
