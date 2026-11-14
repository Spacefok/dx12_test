#include <Windows.h>
#include <exception>
#include <string>
#include <sstream>
#include "Framework.hpp"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR commandLine, int)
{
    try
    {
        std::wistringstream arguments(commandLine ? commandLine : L"");
        std::wstring argument;
        size_t initialScene = 0;
        while (arguments >> argument) {
            if (argument == L"--terrain") initialScene = 4;
            else if (argument == L"--taa-demo") initialScene = 5;
            else if (argument == L"--atmosphere") initialScene = 6;
            else if (argument == L"--rt-shadows") initialScene = 7;
            else if (argument == L"--mesh-pipeline") initialScene = 8;
            else throw std::invalid_argument("Usage: DX12SceneRenderer.exe [--terrain | --taa-demo | --atmosphere | --rt-shadows | --mesh-pipeline]");
        }
        Framework app(1280, 720, L"DX12 Scene Renderer", initialScene);
        if (!app.Init()) return 0;
        return app.Run();
    }
    catch (const std::exception& e)
    {
        MessageBoxA(nullptr, e.what(), "Fatal error", MB_OK | MB_ICONERROR);
        return -1;
    }
    catch (...)
    {
        MessageBoxA(nullptr, "Unknown non-std exception.", "Fatal error", MB_OK | MB_ICONERROR);
        return -2;
    }

}
