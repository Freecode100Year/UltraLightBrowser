#include "PowerManager.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>

int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
int main() {
    using UltraLight::PowerManager;
    auto& power = PowerManager::Instance();
    ICoreWebView2Controller controller;
    ICoreWebView2 web;

    web.lifecycle.successful = FALSE;
    power.HandleWindowMinimize(&controller, &web, false);
    Check(!power.IsSuspended(), "failed asynchronous suspend must not report suspended");
    Check(power.IsBackgrounded(), "failed suspend still needs foreground visibility and QoS restoration");
    Check(controller.visible == FALSE, "non-audio background must hide controller before suspend");
    power.HandleWindowRestore(&controller, &web);

    web.lifecycle.successful = TRUE;
    web.lifecycle.suspendResult = E_FAIL;
    power.HandleWindowMinimize(&controller, &web, false);
    Check(!power.IsSuspended(), "failed TrySuspend call must not leave pending state");
    power.HandleWindowMinimize(&controller, &web, true);
    Check(web.memory.target == COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL,
          "audio after failed suspend must restore normal memory target");
    power.HandleWindowRestore(&controller, &web);
    web.lifecycle.suspendResult = S_OK;
    power.HandleWindowMinimize(&controller, &web, false);
    Check(power.IsSuspended(), "a successful retry must suspend");
    power.HandleWindowRestore(&controller, &web);

    web.lifecycle.beforeCompletion = [&] { power.HandleWindowRestore(&controller, &web); };
    Fake::trims = 0;
    power.HandleWindowMinimize(&controller, &web, false);
    Check(!power.IsSuspended(), "restore before completion must invalidate old suspend result");
    Check(Fake::trims == 0, "stale completion must not trim active window working set");
    Check(web.memory.target == COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL,
          "restore during suspend must leave normal memory target");
    web.lifecycle.beforeCompletion = {};
    power.HandleWindowRestore(&controller, &web);

    const int resumesBeforeAudioRace = web.lifecycle.resumes;
    web.lifecycle.beforeCompletion = [&] { power.HandleWindowMinimize(&controller, &web, true); };
    power.HandleWindowMinimize(&controller, &web, false);
    Check(!power.IsSuspended(), "audio transition must invalidate pending suspend state");
    Check(web.lifecycle.resumes >= resumesBeforeAudioRace + 2,
          "late suspend completion after audio transition must resume audio renderer");
    web.lifecycle.beforeCompletion = {};
    power.HandleWindowRestore(&controller, &web);

    Fake::processes = {{10, 11}, {11, 12}, {12, 13}, {99, 100}};
    Fake::snapshots = 0;
    Fake::throttling.clear();
    power.SetProcessTreeEcoQoS(10, true);
    Check(Fake::snapshots == 1, "process tree traversal must use exactly one system snapshot");
    for (HANDLE pid : {11, 12, 13}) {
        Check(std::count(Fake::throttling.begin(), Fake::throttling.end(), std::pair<HANDLE, bool>{pid, true}) == 1,
              "each descendant must be throttled once");
    }
    Check(Fake::throttling.size() == 3, "unrelated processes must remain untouched");

    Fake::processes = {{10, 11}, {11, 10}, {11, 12}, {11, 12}};
    Fake::throttling.clear();
    power.SetProcessTreeEcoQoS(10, true);
    Check(Fake::throttling.size() == 2, "cycles and duplicate PIDs must not revisit processes or throttle root");
    Fake::processes = {{10, 11}, {11, 12}, {12, 13}, {99, 100}};
    Fake::throttling.clear();
    auto suspendsBeforeAudio = web.lifecycle.suspends;
    power.HandleWindowMinimize(&controller, &web, true);
    Check(web.lifecycle.suspends == suspendsBeforeAudio, "audio background must never call TrySuspend");
    for (HANDLE pid : {10, 11, 12, 13}) {
        Check(std::find(Fake::throttling.begin(), Fake::throttling.end(), std::pair<HANDLE, bool>{pid, false}) != Fake::throttling.end(),
              "audio background must remove throttling from browser and all descendants");
    }
    power.HandleWindowRestore(&controller, &web);
    if (failures) return EXIT_FAILURE;
    std::cout << "Power manager regression tests passed\n";
}
