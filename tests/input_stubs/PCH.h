#pragma once
#include <cmath>
#include <cstdint>
#include <string>
namespace logger {
inline void info(const char*) {}
template<class... A>
void debug(A&&...) {}
}
namespace RE {
using BSFixedString = std::string;
enum class INPUT_EVENT_TYPE { kButton, kThumbstick, kDeviceConnect };
enum class INPUT_DEVICE { kKeyboard, kMouse, kGamepad };
enum class BSEventNotifyControl { kContinue };
template<class T>
struct BSTEventSource {};
template<class T>
struct BSTEventSink {
    virtual ~BSTEventSink() = default;
    virtual BSEventNotifyControl ProcessEvent(T const*, BSTEventSource<T>*) = 0;
};
struct Device {
    INPUT_DEVICE value = INPUT_DEVICE::kKeyboard;
    INPUT_DEVICE get() const { return value; }
};
struct ButtonEvent;
struct InputEvent {
    INPUT_EVENT_TYPE eventType = INPUT_EVENT_TYPE::kButton;
    Device device;
    InputEvent* next = nullptr;
    ButtonEvent* AsButtonEvent();
};
struct ButtonEvent : InputEvent {
    std::uint32_t code = 0;
    bool pressed = false, down = false;
    std::string userEvent;
    auto GetIDCode() const { return code; }
    bool IsDown() const { return down; }
    bool IsUp() const { return !pressed; }
    bool IsPressed() const { return pressed; }
    const auto& QUserEvent() const { return userEvent; }
};
inline ButtonEvent* InputEvent::AsButtonEvent() { return static_cast<ButtonEvent*>(this); }
struct ThumbstickEvent : InputEvent {
    float xValue = 0, yValue = 0;
    bool left = true;
    ThumbstickEvent() {
        eventType = INPUT_EVENT_TYPE::kThumbstick;
        device.value = INPUT_DEVICE::kGamepad;
    }
    bool IsLeft() const { return left; }
};
struct DeviceConnectEvent : InputEvent {
    bool connected = false;
    DeviceConnectEvent() { eventType = INPUT_EVENT_TYPE::kDeviceConnect; }
};
struct Console {
    static constexpr const char* MENU_NAME = "Console";
};
struct UI {
    bool paused = false, console = false;
    static inline UI* instance = nullptr;
    static UI* GetSingleton() { return instance; }
    bool GameIsPaused() const { return paused; }
    bool IsMenuOpen(const char*) const { return console; }
};
struct UserEvents {
    std::string forward = "Forward", back = "Back", strafeLeft = "Left", strafeRight = "Right", sprint = "Sprint",
                jump = "Jump";
    static UserEvents* GetSingleton() {
        static UserEvents instance;
        return &instance;
    }
};
struct BSInputDeviceManager {
    static BSInputDeviceManager* GetSingleton() {
        static BSInputDeviceManager instance;
        return &instance;
    }
    template<class T>
    void AddEventSink(T*) {}
};
struct PlayerCharacter {
    static PlayerCharacter* GetSingleton() {
        static PlayerCharacter instance;
        return &instance;
    }
};
}
