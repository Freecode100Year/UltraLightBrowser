"""Compile and exercise the actual C++ script-registration block with API boundaries."""
from pathlib import Path
import subprocess
import tempfile

source = Path('src/WebViewManager.cpp').read_text()
start = source.index('    const auto generation = ++m_audioScriptGeneration;')
end = source.index('    m_webView->ExecuteScript(wideJs.c_str(), nullptr);', start)
body = source[start:end]
harness = r'''
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <iostream>
using HRESULT = long;
using LPCWSTR = const wchar_t*;
#define SUCCEEDED(x) ((x) >= 0)
#define FAILED(x) ((x) < 0)
constexpr HRESULT S_OK = 0;
struct ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler {
    virtual ~ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler() = default;
    virtual HRESULT Invoke(HRESULT, LPCWSTR) = 0;
    virtual std::unique_ptr<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler> Clone() const = 0;
};
template<class T, class F> struct Holder : T {
    F fn; explicit Holder(F f):fn(f){}
    HRESULT Invoke(HRESULT hr,LPCWSTR id) override {return fn(hr,id);}
    std::unique_ptr<T> Clone() const override {return std::make_unique<Holder<T,F>>(fn);}
    T* Get(){return this;}
};
template<class T,class F> Holder<T,F> Callback(F f){return Holder<T,F>(f);}
struct FakeView {
    std::vector<std::unique_ptr<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>> callbacks;
    std::set<std::wstring> installed;
    int reloads=0; bool rejectCall=false;
    HRESULT AddScriptToExecuteOnDocumentCreated(LPCWSTR,ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler* cb){
        if(rejectCall)return -1;
        callbacks.push_back(cb->Clone());return S_OK;
    }
    void RemoveScriptToExecuteOnDocumentCreated(LPCWSTR id){installed.erase(id);}
    void Reload(){++reloads;}
    void Complete(std::size_t index,HRESULT hr=0){
        const auto id=std::to_wstring(index);
        if(SUCCEEDED(hr))installed.insert(id);
        callbacks[index]->Invoke(hr,id.c_str());
    }
};
struct Manager {
    FakeView* m_webView;
    std::uint64_t m_audioScriptGeneration=0;
    unsigned m_pendingAudioScriptRegistrations=0;
    bool m_audioReloadPending=false;
    std::wstring m_audioScriptId;
    void Register(bool reloadPage){
        std::wstring wideJs=L"script";
'''
harness += body
harness += r'''
    }
};
int failures=0;
void check(bool ok,const char* name){if(!ok){std::cerr<<"FAIL: "<<name<<'\n';++failures;}}
int main(){
    for(bool latestFirst:{false,true}){
        FakeView view;Manager manager{&view};
        manager.Register(true);manager.Register(false);
        view.Complete(latestFirst?1:0);
        check(view.reloads==0,"wait for all registrations before reloading");
        view.Complete(latestFirst?0:1);
        check(view.reloads==1,"superseding preset update must retain mode-switch reload");
        check(view.installed==std::set<std::wstring>{L"1"},"only latest script remains installed");
        check(manager.m_pendingAudioScriptRegistrations==0,"registration count drains");
    }
    {
        FakeView view;Manager manager{&view};
        manager.Register(true);manager.Register(false);
        view.Complete(0);view.Complete(1,-1);
        check(view.reloads==0,"failed latest registration must not reload stale script");
    }
    {
        FakeView view;Manager manager{&view};view.rejectCall=true;
        manager.Register(true);
        check(manager.m_pendingAudioScriptRegistrations==0,"synchronous failure clears pending count");
    }
    if(failures)return 1;
    std::cout<<"Audio registration regression tests passed\n";
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'registration.cpp'
    binary = Path(directory) / 'registration'
    cpp.write_text(harness)
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
