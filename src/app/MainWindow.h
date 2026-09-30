#pragma once

#include "app/Model.h"
#include "app/Shell.h"
#include "ui/Renderer.h"
#include "ui/Theme.h"
#include "ui/Ui.h"

#include <mmdeviceapi.h>

namespace dgmod::app {

class MainWindow {
public:
    static constexpr const wchar_t* kClassName = L"dgmodWindow";

    explicit MainWindow(AppModel& model) : model_(model) {}
    ~MainWindow();
    Result<void> Create(int showCmd);
    int Run();
    // Smoke test: the window opens on another virtual desktop (or off-screen), cycles through the pages, saves a
    // PrintWindow capture of each one into `shotDir` (if not empty) and closes itself.
    void EnableSmokeTest(double seconds, std::wstring shotDir) {
        smokeSeconds_ = seconds;
        smokeDir_ = std::move(shotDir);
    }
    [[nodiscard]] int FramesRendered() const { return frames_; }
    [[nodiscard]] int Captures() const { return captures_; }
    [[nodiscard]] bool OnOtherDesktop() const { return otherDesktop_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Render();
    void Invalidate() { dirty_ = true; }
    void ApplyTheme();
    void SetMouse(LPARAM lp, bool screen);
    [[nodiscard]] float Scale() const { return static_cast<float>(dpi_) / 96.f; }
    [[nodiscard]] double Now() const;
    LRESULT HitTest(LPARAM lp);
    void SmokeStep();
    bool Capture(const std::wstring& path);

    AppModel& model_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    ui::Renderer renderer_;
    ui::Ui ui_;
    ui::Input input_;
    ui::Theme theme_;
    Shell shell_;
    ShellState shellState_;
    bool trackingMouse_ = false;
    bool trackingNc_ = false;
    int64_t startQpc_ = 0;
    int64_t qpcFreq_ = 1;
    bool dirty_ = true;
    bool animating_ = false;
    bool inSizeMove_ = false;
    double smokeSeconds_ = 0;
    std::wstring smokeDir_;
    int smokePage_ = -1;
    int frames_ = 0;
    int captures_ = 0;
    bool otherDesktop_ = false;
    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMNotificationClient> notifier_;
};

}  // namespace dgmod::app
