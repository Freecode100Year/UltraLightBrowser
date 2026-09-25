#pragma once

#include <string>
#include <vector>
#include <windows.h>

namespace UltraLight {

struct DnsProvider {
    std::string id;              // Unique identifier, e.g. "alidns"
    std::wstring name;           // Display name
    std::wstring description;    // Feature & performance description
    std::wstring ipv4Primary;    // Primary IPv4 address
    std::wstring ipv4Secondary;  // Secondary IPv4 address
    std::wstring ipv6Primary;    // Primary IPv6 address
    std::wstring ipv6Secondary;  // Secondary IPv6 address
    std::wstring dohTemplate;    // DoH (DNS-over-HTTPS) URI template
};

class DnsManager {
public:
    static DnsManager& Instance();

    const std::vector<DnsProvider>& GetProviders() const { return m_providers; }
    const DnsProvider* GetProvider(const std::string& id) const;
    const DnsProvider* GetActiveProvider() const;

    // Apply DNS settings to both UserData/Default/Preferences JSON and HKCU Registry Policies
    bool ApplySettings();

    // Show native Win32 interactive dialog for Public DNS configuration
    void ShowDnsDialog(HWND hWndParent);

private:
    DnsManager();
    ~DnsManager() = default;

    DnsManager(const DnsManager&) = delete;
    DnsManager& operator=(const DnsManager&) = delete;

    void InitializeProviders();
    std::vector<DnsProvider> m_providers;
};

} // namespace UltraLight
