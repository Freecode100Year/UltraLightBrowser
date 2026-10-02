#include "UserAgent.hpp"
#include <cassert>
#include <iostream>
int main() {
 const std::wstring original=L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36 Edg/130.0.2849.68";
 const auto mac=UltraLight::BuildUserAgent(original,"macos-edge");
 assert(mac.find(L"(Macintosh; Intel Mac OS X 10_15_7)")!=std::wstring::npos);
 assert(mac.substr(mac.find(L")"))==original.substr(original.find(L")")));
 assert(UltraLight::BuildUserAgent(original,"default")==original);
 assert(UltraLight::BuildUserAgent(original,"invalid")==original);
 assert(UltraLight::BuildUserAgent(L"broken","macos-edge").empty());
 assert(UltraLight::BuildUserAgent(L"Mozilla/5.0 (Windows NT 10.0","macos-edge").empty());
 std::cout << "User agent profile tests passed\n";
}
