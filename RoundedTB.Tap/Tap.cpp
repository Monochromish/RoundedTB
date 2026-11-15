// Loaded into explorer.exe to edit the taskbar's XAML: button-highlight corners, and the battery
// tooltip (plus the quick settings battery button) so a hover shows the charge rate and time left.
//
// Getting in: RoundedTB hooks the taskbar thread with HookProc (which loads this DLL into Explorer)
// and sends the taskbar a registered message. ApplyMessage's wParam is the radius x100 (negative
// restores Explorer's own corners). BatteryMessage's wParam is 1 to append the wattage and 0
// to hand the name back to Explorer. lParam 1 on either message logs to %TEMP%\RoundedTB.Tap.log.
// The first message starts XAML diagnostics on Explorer's own process with this DLL as the TAP;
// later ones just restyle what it has found.
//
// Everything goes through IVisualTreeService rather than typed XAML classes, so the same code works
// whether the taskbar is built on system XAML or WinUI 3.

#include <windows.h>
#include <winioctl.h>
#include <ocidl.h>
#include <xamlOM.h>
#include <batclass.h>
#include <setupapi.h>
#include <winrt/base.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// {5F7C9D1E-6B3A-4C2E-9F1D-8A7B6C5D4E3F}
static const CLSID CLSID_Tap = { 0x5f7c9d1e, 0x6b3a, 0x4c2e, { 0x9f, 0x1d, 0x8a, 0x7b, 0x6c, 0x5d, 0x4e, 0x3f } };

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
    struct Highlight
    {
        winrt::com_ptr<IVisualTreeService3> tree;
        InstanceHandle element;
        unsigned int cornerRadius;
        std::wstring xamlNamespace;
    };

    // Recursive: restyling can raise tree-change callbacks on the same thread.
    std::recursive_mutex lock;
    bool started = false;
    double radius = -1;
    std::wstring logPath;
    std::vector<Highlight> highlights;

    // {72631e54-78a4-11d0-bcf7-00aa00b7b32a} — GUID_DEVICE_BATTERY, copied so poclass.h's extern
    // declaration doesn't have to be linked.
    const GUID BatteryDeviceGuid = { 0x72631e54, 0x78a4, 0x11d0, { 0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a } };

    struct Node
    {
        InstanceHandle parent;
        std::wstring type;
        std::wstring name;
    };

    struct TooltipProp
    {
        bool found = false;
        unsigned int index = 0;
        std::wstring type;
    };

    struct BatteryTarget
    {
        winrt::com_ptr<IVisualTreeService3> tree;
        InstanceHandle element;
        InstanceHandle source;
        unsigned int tooltip = 0;
        std::wstring tooltipType;
        std::wstring last;
        bool fallback = false;
        bool loggedFailure = false;
    };

    std::unordered_map<InstanceHandle, Node> nodes;
    std::vector<BatteryTarget> batteryTargets;
    // Last system-tray icon under each control-center button, used when a build no longer names
    // the battery slot SystemTray.BatteryIconContent. Network, volume, then battery.
    std::unordered_map<InstanceHandle, InstanceHandle> fallbackIcon;
    std::unordered_set<InstanceHandle> typedBatteryButtons;
    bool batteryEnabled = false;
    UINT_PTR batteryTimer = 0;

    void Log(const std::wstring& line)
    {
        if (!logPath.empty())
        {
            std::wofstream(logPath, std::ios::app) << line << std::endl;
        }
    }

    void Style(const Highlight& highlight, double value)
    {
        if (value < 0)
        {
            highlight.tree->ClearProperty(highlight.element, highlight.cornerRadius);
            return;
        }
        InstanceHandle cornerRadius = 0;
        BSTR typeName = SysAllocString((highlight.xamlNamespace + L".CornerRadius").c_str());
        BSTR text = SysAllocString(std::to_wstring(value).c_str());
        if (SUCCEEDED(highlight.tree->CreateInstance(typeName, text, &cornerRadius)))
        {
            highlight.tree->SetProperty(highlight.element, cornerRadius, highlight.cornerRadius);
        }
        SysFreeString(typeName);
        SysFreeString(text);
    }

    bool FindProperty(IVisualTreeService3* tree, InstanceHandle element, const wchar_t* property, unsigned int& index)
    {
        unsigned int sourceCount = 0, propertyCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* values = nullptr;
        if (FAILED(tree->GetPropertyValuesChain(element, &sourceCount, &sources, &propertyCount, &values)))
        {
            return false;
        }
        bool found = false;
        for (unsigned int i = 0; i < propertyCount; i++)
        {
            if (!found && values[i].PropertyName && wcscmp(values[i].PropertyName, property) == 0)
            {
                index = values[i].Index;
                found = true;
            }
            SysFreeString(values[i].Type);
            SysFreeString(values[i].DeclaringType);
            SysFreeString(values[i].ValueType);
            SysFreeString(values[i].ItemType);
            SysFreeString(values[i].Value);
            SysFreeString(values[i].PropertyName);
        }
        for (unsigned int i = 0; i < sourceCount; i++)
        {
            SysFreeString(sources[i].TargetType);
            SysFreeString(sources[i].Name);
        }
        CoTaskMemFree(values);
        CoTaskMemFree(sources);
        return found;
    }

    bool Contains(const std::wstring& text, const wchar_t* needle)
    {
        return text.find(needle) != std::wstring::npos;
    }

    // AutomationProperties.AutomationId, matched exactly so BatterySaver doesn't count as Battery.
    bool AutomationIdIs(IVisualTreeService3* tree, InstanceHandle element, const wchar_t* expected)
    {
        unsigned int sourceCount = 0, propertyCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* values = nullptr;
        if (FAILED(tree->GetPropertyValuesChain(element, &sourceCount, &sources, &propertyCount, &values)))
        {
            return false;
        }
        bool match = false;
        for (unsigned int i = 0; i < propertyCount && !match; i++)
        {
            bool named = values[i].PropertyName && wcscmp(values[i].PropertyName, L"AutomationId") == 0;
            bool fromAutomation = values[i].DeclaringType && wcsstr(values[i].DeclaringType, L"AutomationProperties") != nullptr;
            match = named && fromAutomation && values[i].Value && wcscmp(values[i].Value, expected) == 0;
        }
        for (unsigned int i = 0; i < propertyCount; i++)
        {
            SysFreeString(values[i].Type);
            SysFreeString(values[i].DeclaringType);
            SysFreeString(values[i].ValueType);
            SysFreeString(values[i].ItemType);
            SysFreeString(values[i].Value);
            SysFreeString(values[i].PropertyName);
        }
        for (unsigned int i = 0; i < sourceCount; i++)
        {
            SysFreeString(sources[i].TargetType);
            SysFreeString(sources[i].Name);
        }
        CoTaskMemFree(values);
        CoTaskMemFree(sources);
        return match;
    }

    InstanceHandle FindAncestor(InstanceHandle element, bool (*matches)(const Node&))
    {
        InstanceHandle current = element;
        for (int depth = 0; depth < 12; depth++)
        {
            auto it = nodes.find(current);
            if (it == nodes.end())
            {
                return 0;
            }
            if (matches(it->second))
            {
                return current;
            }
            if (it->second.parent == 0 || it->second.parent == current)
            {
                return 0;
            }
            current = it->second.parent;
        }
        return 0;
    }

    bool IsControlCenterButton(const Node& node)
    {
        return node.name == L"ControlCenterButton";
    }

    bool IsOmniButton(const Node& node)
    {
        return Contains(node.type, L"OmniButton") || node.name == L"ControlCenterButton";
    }

    bool InControlCenter(InstanceHandle element)
    {
        return FindAncestor(element, [](const Node& node) {
            return node.name == L"ControlCenterRegion" || node.name == L"FooterGrid" || Contains(node.type, L"ControlCenter");
        }) != 0;
    }

    std::wstring FormatWatts(long milliwatts)
    {
        long absolute = milliwatts < 0 ? -milliwatts : milliwatts;
        long whole = absolute / 1000;
        long tenth = (absolute % 1000) / 100;
        wchar_t buffer[32];
        if (tenth == 0)
        {
            swprintf_s(buffer, L"%ld W", whole);
        }
        else
        {
            swprintf_s(buffer, L"%ld.%ld W", whole, tenth);
        }
        return buffer;
    }

    // One sample of the system battery. Rate is milliwatts, positive charging. Capacities are mWh.
    struct ChargeInfo
    {
        bool present = false;
        bool ac = false;
        bool charging = false;
        int percent = -1;
        long rate = 0;
        long long remainMwh = 0;
        long long fullMwh = 0;
    };

    // The raw rate flutters by several watts. The estimate uses a short average so the time doesn't jump.
    long samples[5] = {};
    int sampleCount = 0;
    int samplePos = 0;

    long SmoothRate(long rate)
    {
        if (sampleCount > 0)
        {
            long previous = samples[(samplePos + 4) % 5];
            if ((previous > 500 && rate < -500) || (previous < -500 && rate > 500))
            {
                sampleCount = 0;
                samplePos = 0;
            }
        }
        samples[samplePos] = rate;
        samplePos = (samplePos + 1) % 5;
        if (sampleCount < 5)
        {
            sampleCount++;
        }
        long long sum = 0;
        for (int i = 0; i < sampleCount; i++)
        {
            int index = (samplePos - 1 - i + 5) % 5;
            sum += samples[index];
        }
        return static_cast<long>(sum / sampleCount);
    }

    std::wstring FormatSpan(long long minutes)
    {
        if (minutes < 1)
        {
            return L"under a minute";
        }
        if (minutes >= 10)
        {
            minutes = ((minutes + 2) / 5) * 5;
        }
        long long hours = minutes / 60;
        long long mins = minutes % 60;
        std::wstring span;
        if (hours > 0)
        {
            span += std::to_wstring(hours) + L" hr";
        }
        if (mins > 0)
        {
            if (!span.empty())
            {
                span += L" ";
            }
            span += std::to_wstring(mins) + L" min";
        }
        return span.empty() ? L"under a minute" : span;
    }

    // "About 2 hr 15 min to full", or the matching line while on battery. Empty when the rate is noise.
    std::wstring FormatEstimate(const ChargeInfo& info)
    {
        if (info.percent >= 99 && (info.ac || info.charging))
        {
            return L"Fully charged";
        }
        if (info.fullMwh <= 0 || info.remainMwh < 0)
        {
            return L"";
        }
        long long minutes = -1;
        bool toFull = false;
        if ((info.ac || info.charging) && info.rate > 1500 && info.fullMwh > info.remainMwh)
        {
            toFull = true;
            minutes = ((info.fullMwh - info.remainMwh) * 60) / info.rate;
        }
        else if (!info.ac && !info.charging && info.rate < -1500 && info.remainMwh > 0)
        {
            minutes = (info.remainMwh * 60) / static_cast<long long>(-info.rate);
        }
        if (minutes < 0)
        {
            return L"";
        }
        if (minutes > 24 * 60)
        {
            return toFull ? L"More than a day to full" : L"More than a day left";
        }
        std::wstring span = FormatSpan(minutes);
        if (span == L"under a minute")
        {
            return toFull ? L"Under a minute to full" : L"Under a minute left";
        }
        return L"About " + span + (toFull ? L" to full" : L" left");
    }

    // The bubble is rewritten around the live reading. Windows' own warning (a slow charger, for
    // example) is kept as the last line.
    std::wstring ComposeTooltip(const ChargeInfo& info, const std::wstring& note)
    {
        std::wstring text;
        if (info.percent >= 0)
        {
            text = std::to_wstring(info.percent) + L"% available";
            text += (info.ac || info.charging) ? L" \u00B7 plugged in" : L" \u00B7 on battery";
        }
        std::wstring power;
        if ((info.ac || info.charging) && info.rate > 1000)
        {
            power = L"Charging at " + FormatWatts(info.rate);
        }
        else if (!info.ac && !info.charging && info.rate < -1000)
        {
            power = L"Using " + FormatWatts(info.rate);
        }
        std::wstring estimate = FormatEstimate(info);
        if (!power.empty())
        {
            if (!text.empty())
            {
                text += L"\n";
            }
            text += power;
        }
        if (!estimate.empty() && estimate != L"Fully charged")
        {
            if (!text.empty())
            {
                text += L"\n";
            }
            text += estimate;
        }
        else if (estimate == L"Fully charged")
        {
            if (!text.empty())
            {
                text += L"\n";
            }
            text += estimate;
        }
        if (!note.empty())
        {
            if (!text.empty())
            {
                text += L"\n\n";
            }
            text += note;
        }
        return text;
    }

    bool IsGeneratedLine(const std::wstring& line)
    {
        if (line.rfind(L"Battery status:", 0) == 0 || line.rfind(L"Power Battery", 0) == 0)
        {
            return true;
        }
        if (line.rfind(L"Charging at ", 0) == 0 || line.rfind(L"Using ", 0) == 0 || line.rfind(L"About ", 0) == 0)
        {
            return true;
        }
        if (line.rfind(L"Under a minute", 0) == 0 || line.rfind(L"Fully charged", 0) == 0 || line.rfind(L"More than a day", 0) == 0)
        {
            return true;
        }
        size_t i = 0;
        while (i < line.size() && line[i] >= L'0' && line[i] <= L'9')
        {
            i++;
        }
        return i > 0 && line.compare(i, 11, L"% available") == 0;
    }

    // Lines Windows wrote that aren't the stock status sentence.
    std::wstring ExtractShellNote(const std::wstring& text)
    {
        std::wstring note;
        size_t start = 0;
        while (start <= text.size())
        {
            size_t end = text.find(L'\n', start);
            std::wstring line = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!line.empty() && line.back() == L'\r')
            {
                line.pop_back();
            }
            if (!line.empty() && !IsGeneratedLine(line))
            {
                if (!note.empty())
                {
                    note += L"\n";
                }
                note += line;
            }
            if (end == std::wstring::npos)
            {
                break;
            }
            start = end + 1;
        }
        return note;
    }

    // Live charge rate from the battery driver. present is false when this PC has no battery.
    ChargeInfo ReadCharge()
    {
        HDEVINFO devices = SetupDiGetClassDevsW(&BatteryDeviceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devices == INVALID_HANDLE_VALUE)
        {
            return {};
        }

        struct Reading
        {
            bool system;
            bool ac;
            bool charging;
            bool discharging;
            bool rateKnown;
            long rate;
            bool relative;
            ULONG capacity;
            ULONG full;
        };
        std::vector<Reading> readings;

        SP_DEVICE_INTERFACE_DATA iface = {};
        iface.cbSize = sizeof(iface);
        for (DWORD index = 0; SetupDiEnumDeviceInterfaces(devices, nullptr, &BatteryDeviceGuid, index, &iface); index++)
        {
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailW(devices, &iface, nullptr, 0, &needed, nullptr);
            if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
            {
                continue;
            }
            std::vector<BYTE> buffer(needed);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, needed, nullptr, nullptr))
            {
                continue;
            }

            HANDLE battery = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (battery == INVALID_HANDLE_VALUE)
            {
                continue;
            }

            ULONG timeout = 0;
            ULONG tag = 0;
            DWORD got = 0;
            BOOL tagged = DeviceIoControl(battery, IOCTL_BATTERY_QUERY_TAG, &timeout, sizeof(timeout), &tag, sizeof(tag), &got, nullptr);
            if (!tagged || tag == BATTERY_TAG_INVALID)
            {
                CloseHandle(battery);
                continue;
            }

            BATTERY_QUERY_INFORMATION query = {};
            query.BatteryTag = tag;
            query.InformationLevel = BatteryInformation;
            BATTERY_INFORMATION info = {};
            BOOL described = DeviceIoControl(battery, IOCTL_BATTERY_QUERY_INFORMATION, &query, sizeof(query), &info, sizeof(info), &got, nullptr);

            BATTERY_WAIT_STATUS wait = {};
            wait.BatteryTag = tag;
            BATTERY_STATUS status = {};
            BOOL measured = DeviceIoControl(battery, IOCTL_BATTERY_QUERY_STATUS, &wait, sizeof(wait), &status, sizeof(status), &got, nullptr);
            CloseHandle(battery);
            if (!described || !measured)
            {
                continue;
            }

            Reading reading = {};
            reading.system = (info.Capabilities & BATTERY_SYSTEM_BATTERY) != 0;
            reading.ac = (status.PowerState & BATTERY_POWER_ON_LINE) != 0;
            reading.charging = (status.PowerState & BATTERY_CHARGING) != 0;
            reading.discharging = (status.PowerState & BATTERY_DISCHARGING) != 0;
            reading.rateKnown = status.Rate != static_cast<LONG>(BATTERY_UNKNOWN_RATE);
            reading.rate = status.Rate;
            reading.relative = (info.Capabilities & BATTERY_CAPACITY_RELATIVE) != 0;
            reading.capacity = status.Capacity;
            reading.full = info.FullChargedCapacity;
            if (reading.full == BATTERY_UNKNOWN_CAPACITY || reading.full == 0)
            {
                reading.full = info.DesignedCapacity;
            }
            readings.push_back(reading);
        }
        SetupDiDestroyDeviceInfoList(devices);
        if (readings.empty())
        {
            return {};
        }

        bool anySystem = false;
        for (const Reading& reading : readings)
        {
            anySystem = anySystem || reading.system;
        }

        bool ac = false;
        bool charging = false;
        bool rateKnown = false;
        long rate = 0;
        unsigned long long remain = 0;
        unsigned long long full = 0;
        bool absolute = false;
        int relativePercent = -1;
        for (const Reading& reading : readings)
        {
            if (anySystem && !reading.system)
            {
                continue;
            }
            ac = ac || reading.ac;
            charging = charging || reading.charging;
            if (reading.rateKnown)
            {
                rateKnown = true;
                rate += reading.rate;
            }
            if (reading.relative)
            {
                if (relativePercent < 0 && reading.capacity <= 100)
                {
                    relativePercent = static_cast<int>(reading.capacity);
                }
            }
            else if (reading.full != BATTERY_UNKNOWN_CAPACITY && reading.full > 0 && reading.capacity != BATTERY_UNKNOWN_CAPACITY)
            {
                absolute = true;
                remain += reading.capacity;
                full += reading.full;
            }
        }

        int percent = -1;
        if (absolute && full > 0)
        {
            percent = static_cast<int>((remain * 100) / full);
        }
        else if (relativePercent >= 0)
        {
            percent = relativePercent;
        }
        if (percent > 100)
        {
            percent = 100;
        }

        ChargeInfo info;
        info.present = true;
        info.ac = ac;
        info.charging = charging;
        info.percent = percent;
        info.rate = rateKnown ? SmoothRate(rate) : 0;
        if (absolute)
        {
            info.remainMwh = static_cast<long long>(remain);
            info.fullMwh = static_cast<long long>(full);
        }
        return info;
    }

    std::wstring ReadPropertyValue(IVisualTreeService3* tree, InstanceHandle element, unsigned int index)
    {
        unsigned int sourceCount = 0, propertyCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* values = nullptr;
        std::wstring result;
        if (FAILED(tree->GetPropertyValuesChain(element, &sourceCount, &sources, &propertyCount, &values)))
        {
            return result;
        }
        for (unsigned int i = 0; i < propertyCount; i++)
        {
            if (values[i].Index == index && values[i].Value && wcscmp(values[i].Value, L"0") != 0)
            {
                result = values[i].Value;
            }
            SysFreeString(values[i].Type);
            SysFreeString(values[i].DeclaringType);
            SysFreeString(values[i].ValueType);
            SysFreeString(values[i].ItemType);
            SysFreeString(values[i].Value);
            SysFreeString(values[i].PropertyName);
        }
        for (unsigned int i = 0; i < sourceCount; i++)
        {
            SysFreeString(sources[i].TargetType);
            SysFreeString(sources[i].Name);
        }
        CoTaskMemFree(values);
        CoTaskMemFree(sources);
        return result;
    }

    bool SetTooltipText(IVisualTreeService3* tree, InstanceHandle element, unsigned int index, const std::wstring& reportedType, const std::wstring& text)
    {
        std::vector<std::wstring> types;
        if (Contains(reportedType, L"String"))
        {
            types.push_back(reportedType);
        }
        for (const wchar_t* fallback : { L"Windows.Foundation.String", L"System.String" })
        {
            if (std::find(types.begin(), types.end(), fallback) == types.end())
            {
                types.emplace_back(fallback);
            }
        }
        for (const std::wstring& type : types)
        {
            InstanceHandle value = 0;
            BSTR typeName = SysAllocString(type.c_str());
            BSTR textValue = SysAllocString(text.c_str());
            HRESULT created = tree->CreateInstance(typeName, textValue, &value);
            SysFreeString(typeName);
            SysFreeString(textValue);
            if (SUCCEEDED(created) && SUCCEEDED(tree->SetProperty(element, value, index)))
            {
                return true;
            }
        }
        return false;
    }

    void ApplyBatteryText(const ChargeInfo& info)
    {
        for (BatteryTarget& target : batteryTargets)
        {
            if (!batteryEnabled || !info.present)
            {
                if (!target.last.empty())
                {
                    target.tree->ClearProperty(target.element, target.tooltip);
                    target.last.clear();
                }
                continue;
            }
            std::wstring current = ReadPropertyValue(target.tree.get(), target.element, target.tooltip);
            std::wstring next = ComposeTooltip(info, ExtractShellNote(current));
            if (next.empty())
            {
                continue;
            }
            if (next == current)
            {
                target.last = next;
                continue;
            }
            if (SetTooltipText(target.tree.get(), target.element, target.tooltip, target.tooltipType, next))
            {
                target.last = next;
                Log(L"Battery tip -> " + next);
            }
            else if (!target.loggedFailure)
            {
                target.loggedFailure = true;
                Log(L"Battery tip set failed, property type " + target.tooltipType);
            }
        }
    }

    void AddBatteryTarget(IVisualTreeService3* tree, InstanceHandle element, InstanceHandle source, const TooltipProp& tooltip, bool fallback)
    {
        for (const BatteryTarget& target : batteryTargets)
        {
            if (target.element == element)
            {
                return;
            }
        }
        winrt::com_ptr<IVisualTreeService3> service;
        service.copy_from(tree);
        batteryTargets.push_back(BatteryTarget{ service, element, source, tooltip.index, tooltip.type, L"", fallback });
        Log(L"Battery target " + std::to_wstring(element) + (fallback ? L" (fallback)" : L""));
        if (batteryEnabled)
        {
            ApplyBatteryText(ReadCharge());
        }
    }

    // The battery slot's hover text is AutomationProperties.Name. The chain lists that property
    // twice; the one with a real string is the text Windows shows.
    bool FindStatusName(IVisualTreeService3* tree, InstanceHandle element, TooltipProp& name)
    {
        unsigned int sourceCount = 0, propertyCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* values = nullptr;
        if (FAILED(tree->GetPropertyValuesChain(element, &sourceCount, &sources, &propertyCount, &values)))
        {
            return false;
        }
        size_t best = 0;
        name = {};
        for (unsigned int i = 0; i < propertyCount; i++)
        {
            // The chain names the attached property "AutomationProperties.Name". DeclaringType is
            // Windows.UI.Xaml.DependencyObject, and a second entry for the same name is the unset "0".
            const wchar_t* property = values[i].PropertyName ? values[i].PropertyName : L"";
            bool attachedName = wcscmp(property, L"AutomationProperties.Name") == 0;
            bool declaredName = wcscmp(property, L"Name") == 0 && values[i].DeclaringType && wcsstr(values[i].DeclaringType, L"AutomationProperties") != nullptr;
            size_t length = values[i].Value ? wcslen(values[i].Value) : 0;
            if ((attachedName || declaredName) && length > best && wcscmp(values[i].Value, L"0") != 0)
            {
                best = length;
                name.found = true;
                name.index = values[i].Index;
                const wchar_t* type = nullptr;
                if (values[i].ValueType && wcsstr(values[i].ValueType, L"String") != nullptr)
                {
                    type = values[i].ValueType;
                }
                else if (values[i].Type && wcsstr(values[i].Type, L"String") != nullptr)
                {
                    type = values[i].Type;
                }
                name.type = type ? type : L"Windows.Foundation.String";
            }
            SysFreeString(values[i].Type);
            SysFreeString(values[i].DeclaringType);
            SysFreeString(values[i].ValueType);
            SysFreeString(values[i].ItemType);
            SysFreeString(values[i].Value);
            SysFreeString(values[i].PropertyName);
        }
        for (unsigned int i = 0; i < sourceCount; i++)
        {
            SysFreeString(sources[i].TargetType);
            SysFreeString(sources[i].Name);
        }
        CoTaskMemFree(values);
        CoTaskMemFree(sources);
        if (!name.found)
        {
            unsigned int index = 0;
            if (SUCCEEDED(tree->GetPropertyIndex(element, L"AutomationProperties.Name", &index)))
            {
                name.found = true;
                name.index = index;
                name.type = L"Windows.Foundation.String";
            }
        }
        return name.found;
    }

    // The hover target is the battery slot, not the whole control-center button, so volume and
    // network keep their own tooltips. Stop before that button.
    void ConsiderBatteryIcon(IVisualTreeService3* tree, InstanceHandle source)
    {
        InstanceHandle slot = source;
        InstanceHandle current = source;
        for (int depth = 0; depth < 8; depth++)
        {
            auto it = nodes.find(current);
            if (it == nodes.end())
            {
                break;
            }
            if (IsOmniButton(it->second) && current != source)
            {
                break;
            }
            if (it->second.name == L"SystemTrayIcon" || Contains(it->second.type, L"IconView"))
            {
                slot = current;
            }
            if (it->second.parent == 0 || it->second.parent == current)
            {
                break;
            }
            current = it->second.parent;
        }
        TooltipProp name;
        InstanceHandle target = 0;
        if (FindStatusName(tree, slot, name))
        {
            target = slot;
        }
        else if (slot != source && FindStatusName(tree, source, name))
        {
            target = source;
        }
        if (target != 0)
        {
            AddBatteryTarget(tree, target, source, name, false);
        }
        else
        {
            Log(L"Battery icon has no accessible name");
        }
    }

    void DropFallback(InstanceHandle button)
    {
        auto it = fallbackIcon.find(button);
        if (it == fallbackIcon.end())
        {
            return;
        }
        InstanceHandle icon = it->second;
        fallbackIcon.erase(it);
        std::erase_if(batteryTargets, [&](BatteryTarget& target) {
            if (!target.fallback || target.element != icon)
            {
                return false;
            }
            if (!target.last.empty())
            {
                target.tree->ClearProperty(target.element, target.tooltip);
            }
            return true;
        });
    }

    // The bubble's words, not a short token that merely occurs inside them. "2" is inside "27%".
    bool IsBatteryHoverText(const std::wstring& text)
    {
        std::wstring body = text;
        if (body.rfind(L"Power ", 0) == 0)
        {
            body = body.substr(6);
        }
        if (body.size() < 12)
        {
            return false;
        }
        return Contains(body, L"attery") || Contains(body, L"lugged in");
    }

    // ToolTip.Content is the string the bubble draws. The icon's accessible name is a different
    // string, and the template's DataContext just echoes Content.
    void TryPatchTooltip(IVisualTreeService3* tree, InstanceHandle element)
    {
        if (!batteryEnabled || nodes.find(element) == nodes.end() || !nodes[element].type.ends_with(L".ToolTip"))
        {
            return;
        }
        for (const BatteryTarget& target : batteryTargets)
        {
            if (target.element == element)
            {
                return;
            }
        }

        unsigned int sourceCount = 0, propertyCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* values = nullptr;
        if (FAILED(tree->GetPropertyValuesChain(element, &sourceCount, &sources, &propertyCount, &values)))
        {
            return;
        }
        TooltipProp prop;
        for (unsigned int i = 0; i < propertyCount && !prop.found; i++)
        {
            bool content = values[i].PropertyName && wcscmp(values[i].PropertyName, L"Content") == 0;
            if (content && values[i].Value && IsBatteryHoverText(values[i].Value))
            {
                prop.found = true;
                prop.index = values[i].Index;
                const wchar_t* type = nullptr;
                if (values[i].ValueType && wcsstr(values[i].ValueType, L"String") != nullptr)
                {
                    type = values[i].ValueType;
                }
                else if (values[i].Type && wcsstr(values[i].Type, L"String") != nullptr)
                {
                    type = values[i].Type;
                }
                prop.type = type ? type : L"Windows.Foundation.String";
            }
        }
        for (unsigned int i = 0; i < propertyCount; i++)
        {
            SysFreeString(values[i].Type);
            SysFreeString(values[i].DeclaringType);
            SysFreeString(values[i].ValueType);
            SysFreeString(values[i].ItemType);
            SysFreeString(values[i].Value);
            SysFreeString(values[i].PropertyName);
        }
        for (unsigned int i = 0; i < sourceCount; i++)
        {
            SysFreeString(sources[i].TargetType);
            SysFreeString(sources[i].Name);
        }
        CoTaskMemFree(values);
        CoTaskMemFree(sources);
        if (prop.found)
        {
            AddBatteryTarget(tree, element, element, prop, false);
        }
    }

    // Content is often assigned a moment after the tooltip is built.
    void ScheduleTooltipProbe(IVisualTreeService3* tree, InstanceHandle element)
    {
        winrt::com_ptr<IVisualTreeService3> service;
        service.copy_from(tree);
        std::thread([service, element]() {
            for (int delay : { 40, 150, 400, 900, 1600 })
            {
                Sleep(delay);
                std::lock_guard guard(lock);
                TryPatchTooltip(service.get(), element);
            }
        }).detach();
    }

    void CALLBACK OnBatteryTimer(HWND, UINT, UINT_PTR, DWORD)
    {
        ChargeInfo info = ReadCharge();
        std::lock_guard guard(lock);
        if (batteryEnabled)
        {
            ApplyBatteryText(info);
        }
    }

    void EnsureBatteryTimer()
    {
        if (batteryTimer == 0)
        {
            batteryTimer = SetTimer(nullptr, 0, 2000, OnBatteryTimer);
        }
    }

    void StopBatteryTimer()
    {
        if (batteryTimer != 0)
        {
            KillTimer(nullptr, batteryTimer);
            batteryTimer = 0;
        }
    }
}

struct Tap : winrt::implements<Tap, IObjectWithSite, IVisualTreeServiceCallback2>
{
    winrt::com_ptr<IXamlDiagnostics> diagnostics;
    winrt::com_ptr<IVisualTreeService3> tree;

    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override
    {
        diagnostics = nullptr;
        tree = nullptr;
        if (!site || FAILED(site->QueryInterface(IID_PPV_ARGS(diagnostics.put()))))
        {
            return site ? E_FAIL : S_OK;
        }
        tree = diagnostics.as<IVisualTreeService3>();

        // Advising on the calling thread deadlocks. The existing tree is replayed as Add events.
        winrt::com_ptr<IVisualTreeService3> service = tree;
        winrt::com_ptr<Tap> self;
        self.copy_from(this);
        std::thread([service, self]() { service->AdviseVisualTreeChange(self.get()); }).detach();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** site) noexcept override
    {
        return diagnostics ? diagnostics->QueryInterface(riid, site) : E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation, VisualElement element, VisualMutationType mutation) noexcept override
    {
        std::lock_guard guard(lock);
        if (mutation == Remove)
        {
            std::erase_if(highlights, [&](const Highlight& h) { return h.element == element.Handle; });
            std::erase_if(batteryTargets, [&](BatteryTarget& target) {
                if (target.element != element.Handle && target.source != element.Handle)
                {
                    return false;
                }
                if (target.element != element.Handle && !target.last.empty())
                {
                    target.tree->ClearProperty(target.element, target.tooltip);
                }
                return true;
            });
            for (auto it = fallbackIcon.begin(); it != fallbackIcon.end();)
            {
                if (it->first == element.Handle || it->second == element.Handle)
                {
                    it = fallbackIcon.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            typedBatteryButtons.erase(element.Handle);
            nodes.erase(element.Handle);
            return S_OK;
        }

        std::wstring type = element.Type ? element.Type : L"";
        std::wstring name = element.Name ? element.Name : L"";
        Log(std::to_wstring(relation.Parent) + L" -> " + std::to_wstring(element.Handle) + L" " + type + L" #" + name);
        nodes[element.Handle] = Node{ relation.Parent, type, name };

        // Hovering the battery opens a tooltip whose text is not the icon's accessible name.
        if (batteryEnabled && type.ends_with(L".ToolTip"))
        {
            TryPatchTooltip(tree.get(), element.Handle);
            ScheduleTooltipProbe(tree.get(), element.Handle);
        }

        // The hover/pressed highlight of every taskbar button, Start and the tray included.
        if (name == L"BackgroundElement" && type.ends_with(L".Xaml.Controls.Border"))
        {
            unsigned int index;
            if (FindProperty(tree.get(), element.Handle, L"CornerRadius", index))
            {
                Highlight highlight{ tree, element.Handle, index, type.substr(0, type.find(L".Controls.")) };
                highlights.push_back(highlight);
                Style(highlight, radius);
            }
        }

        // The tray battery icon. Its class is SystemTray.BatteryIconContent; some builds drop that
        // name, and the last icon inside the control-center button is the battery.
        bool typedBattery = Contains(type, L"BatteryIcon") || Contains(name, L"BatteryIcon");
        if (typedBattery)
        {
            InstanceHandle button = FindAncestor(element.Handle, IsControlCenterButton);
            if (button != 0)
            {
                typedBatteryButtons.insert(button);
                DropFallback(button);
            }
            ConsiderBatteryIcon(tree.get(), element.Handle);
        }
        else if (Contains(type, L"Button") && InControlCenter(element.Handle) && AutomationIdIs(tree.get(), element.Handle, L"Microsoft.QuickAction.Battery"))
        {
            // The battery button in quick settings, the control center's footer.
            TooltipProp statusName;
            if (FindStatusName(tree.get(), element.Handle, statusName))
            {
                AddBatteryTarget(tree.get(), element.Handle, element.Handle, statusName, false);
            }
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) noexcept override
    {
        return S_OK;
    }
};

struct TapFactory : winrt::implements<TapFactory, IClassFactory>
{
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** result) noexcept override
    {
        *result = nullptr;
        if (outer)
        {
            return CLASS_E_NOAGGREGATION;
        }
        try
        {
            return winrt::make<Tap>().as<IUnknown>()->QueryInterface(riid, result);
        }
        catch (...)
        {
            return winrt::to_hresult();
        }
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override
    {
        return S_OK;
    }
};

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** result)
{
    if (clsid != CLSID_Tap)
    {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    try
    {
        return winrt::make<TapFactory>().as<IUnknown>()->QueryInterface(riid, result);
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

// Explorer keeps the TAP for its lifetime; the module must never unload under it.
STDAPI DllCanUnloadNow()
{
    return S_FALSE;
}

static void Start()
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), self, MAX_PATH);

    // The taskbar hosts both system XAML and WinUI 3; attach to whichever are loaded. The endpoint
    // must be one of the framework's own connection names, and ERROR_NOT_FOUND means "try the next".
    using InitializeXamlDiagnosticsExFn = HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);
    struct Framework { const wchar_t* module; const wchar_t* endpoint; };
    for (Framework framework : { Framework{ L"Microsoft.Internal.FrameworkUdk.dll", L"WinUIVisualDiagConnection" }, Framework{ L"Windows.UI.Xaml.dll", L"VisualDiagConnection" } })
    {
        HMODULE module = GetModuleHandleW(framework.module);
        auto initialize = module ? reinterpret_cast<InitializeXamlDiagnosticsExFn>(GetProcAddress(module, "InitializeXamlDiagnosticsEx")) : nullptr;
        if (!initialize)
        {
            continue;
        }
        HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        for (int attempt = 1; attempt <= 1000 && hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND); attempt++)
        {
            hr = initialize((framework.endpoint + std::to_wstring(attempt)).c_str(), GetCurrentProcessId(), L"", self, CLSID_Tap, nullptr);
        }
        std::lock_guard guard(lock);
        Log(std::wstring(framework.module) + L": InitializeXamlDiagnosticsEx hr=" + std::to_wstring(hr));
    }
}

extern "C" __declspec(dllexport) LRESULT CALLBACK HookProc(int code, WPARAM wParam, LPARAM lParam)
{
    static const UINT applyMessage = RegisterWindowMessageW(L"RoundedTB.Tap.Apply");
    static const UINT batteryMessage = RegisterWindowMessageW(L"RoundedTB.Tap.Battery");
    auto* message = reinterpret_cast<CWPSTRUCT*>(lParam);
    if (code == HC_ACTION && (message->message == applyMessage || message->message == batteryMessage))
    {
        // Read the battery outside the lock: the driver call is slow next to a visual-tree callback.
        bool battery = message->message == batteryMessage;
        bool enableBattery = battery && message->wParam != 0;
        ChargeInfo charge = enableBattery ? ReadCharge() : ChargeInfo{};

        std::lock_guard guard(lock);
        if (message->lParam == 1)
        {
            wchar_t temp[MAX_PATH];
            GetTempPathW(MAX_PATH, temp);
            logPath = std::wstring(temp) + L"RoundedTB.Tap.log";
        }
        if (battery)
        {
            batteryEnabled = enableBattery;
        }
        else
        {
            radius = static_cast<double>(static_cast<INT_PTR>(message->wParam)) / 100.0;
        }
        if (!started)
        {
            started = true;
            std::thread(Start).detach();
        }
        else if (!battery)
        {
            for (const Highlight& highlight : highlights)
            {
                Style(highlight, radius);
            }
        }
        if (battery)
        {
            if (enableBattery)
            {
                EnsureBatteryTimer();
                ApplyBatteryText(charge);
            }
            else
            {
                StopBatteryTimer();
                ApplyBatteryText({});
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}
