#include <array>
#include <codecvt>
#include <iostream>
#include <locale>
#include <string>
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <wininet.h>
#include <ws2tcpip.h>
#include <commctrl.h>
// clang-format on
#include <fstream>

// Have to include getopt in the compilation unit or else its compiled with the wrong runtime
#include "getopt.h"
#include "json.hpp"
#include "tagname.h"

void startProcessWrapper(std::string name, std::string args, boolean wait) {
    HANDLE stdout_handle = NULL;
    STARTUPINFO startupInfo = {sizeof(startupInfo)};
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;
    startupInfo.hStdOutput = stdout_handle;
    PROCESS_INFORMATION processInfo;
    LPSTR cmdArgs = const_cast<char*>(args.c_str());
    if (!CreateProcess(name.c_str(), cmdArgs, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL,
                       &startupInfo, &processInfo)) {
        std::cout << "CreateProcess failed (" << GetLastError() << ")" << std::endl;
        CloseHandle(processInfo.hProcess);
        CloseHandle(processInfo.hThread);
    }
    if (wait) {
        WaitForSingleObject(&processInfo.hProcess, INFINITE);
    }
    CloseHandle(processInfo.hProcess);
    CloseHandle(processInfo.hThread);
}

void displayWelcome() {
    MessageBoxA(NULL, "You are currently running on the channel: Bleeding Edge.\n"
                      "This is a build of Citra with additional changes that have not yet been "
                      "fully tested or deemed stable. "
                      "Please do not report bugs against this build.\n\n"
                      "If you believe you've found a bug, please retest on our nightly builds.\n\n"
                      "Citra will automatically prompt you to update if one is available.",
                "Citra", MB_OK | MB_ICONINFORMATION);
}

void installMSVCRuntime(const char* msvc_redist_exe) {
    // this updater is linked statically with the runtime so it should be safe to run on any system
    // TODO change this to support 32 bit as well?
    // check to see if they have a recent enough version of the runtime
    WCHAR szBuffer[512];
    DWORD dwBufferSize = sizeof(szBuffer);
    HKEY hKey;
    LONG lRes = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Wow6432Node\\Microsoft\\DevDiv\\vc\\Servicing\\14.0\\RuntimeMinimum", 0,
        KEY_READ, &hKey);
    if (RegQueryValueExW(hKey, L"Version", 0, NULL, (LPBYTE)szBuffer, &dwBufferSize)) {
        // The installer uses a UAC Prompt so we need to use ShellExecuteEx to wait for it to finish
        SHELLEXECUTEINFO shExInfo = {0};
        shExInfo.cbSize = sizeof(shExInfo);
        shExInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
        shExInfo.hwnd = 0;
        shExInfo.lpVerb = "runas";
        shExInfo.lpFile = msvc_redist_exe;
        shExInfo.lpParameters = "/silent /install /quiet";
        shExInfo.lpDirectory = 0;
        shExInfo.nShow = SW_HIDE;
        shExInfo.hInstApp = 0;
        if (ShellExecuteEx(&shExInfo)) {
            WaitForSingleObject(shExInfo.hProcess, INFINITE);
        }
        CloseHandle(shExInfo.hProcess);
    }
}

// Search for the previous install (if there is one) and copy the user directory into the new
// install
// TODO: Use the version info that squirrel gives us select the right folder
void copyPreviousInstallsUserFolder(std::string path) {
    WIN32_FIND_DATA found_file;
    std::string old_user_dir;
    std::string new_user_dir;
    SHFILEOPSTRUCT s = {0};
    HANDLE h;
    std::vector<std::string> app_folder_paths{""};
    std::vector<__int64> folder_creation_time{0};
    std::string user_folder_old;
    std::string user_folder_new;
    const char* search_path;
    DWORD ftyp;
    std::ofstream myfile;
    myfile.open(".\\log.txt");
    myfile << std::endl
           << "----------" << std::endl
           << "Starting copy of old user folder " << std::endl;
    myfile << "path " << path << std::endl;

    search_path = (path.substr(0, path.find_last_of("\\/")) + "\\*").c_str();
    myfile << "search path " << search_path << std::endl;
    h = FindFirstFileEx(search_path, FindExInfoStandard, &found_file,
                        FindExSearchLimitToDirectories, NULL, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (found_file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                std::string fname(found_file.cFileName);
                // find the app folders. It can be 2 or 3 depending on if this is a first time
                // upgrade, or a subsequent upgrade
                if (fname.substr(0, 3) == "app") {
                    ULARGE_INTEGER ul;
                    ul.LowPart = found_file.ftCreationTime.dwLowDateTime;
                    ul.HighPart = found_file.ftCreationTime.dwHighDateTime;
                    app_folder_paths.push_back(path.substr(0, path.find_last_of("\\/")) + "\\" +
                                               fname);
                    // get the creation time for this folder. we want to make sure we are copying
                    // over from the second
                    // most recently created folder (the previous version) into the latest version
                    folder_creation_time.push_back(ul.QuadPart);
                }
            }
        } while (FindNextFile(h, &found_file));
        FindClose(h);
    }
    // We have two lists, one with timestamps and the other with the actual lists. TODO use a map.
    // Try to find the newest (the currently installing version) and the second newest (the previous
    // version)
    int newest_index = 0;
    int second_newest_index = 0;
    for (int i = 1; i < folder_creation_time.size(); ++i) {
        if (folder_creation_time[i] > folder_creation_time[newest_index]) {
            second_newest_index = newest_index;
            newest_index = i;
        }
    }

    myfile << "app_folder_paths " << std::endl;
    for (const auto& i : app_folder_paths) {
        myfile << '\t' << i << std::endl;
    }
    myfile << "folder_creation_time " << std::endl;
    for (const auto& i : folder_creation_time) {
        myfile << '\t' << i << std::endl;
    }
    myfile << "newest_index " << newest_index << std::endl;
    myfile << "oldest_index " << second_newest_index << std::endl;
    user_folder_old = app_folder_paths[second_newest_index] + "\\user";
    user_folder_new = app_folder_paths[newest_index] + "\\user";
    myfile << "old " << user_folder_old << std::endl;
    myfile << "new " << user_folder_new << std::endl;

    // See if the old directory even has a user folder. (Note: It should unless they deleted it)
    ftyp = GetFileAttributesA(user_folder_old.c_str());
    if (ftyp != INVALID_FILE_ATTRIBUTES && (ftyp & FILE_ATTRIBUTE_DIRECTORY)) {
        // copy the old user directory over. Yes. SHFileOperation requires double null terminated
        // strings
        // TODO: Remove SHFileOperation and change it to IFileOperation
        user_folder_old += "\\*";
        const char* copy_from = user_folder_old.c_str() + '\0' + '\0';
        const char* copy_to = user_folder_new.c_str() + '\0' + '\0';
        s.hwnd = nullptr;
        s.wFunc = FO_COPY;
        s.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR;
        s.pTo = copy_to;
        s.pFrom = copy_from;
        int result = SHFileOperation(&s);
        myfile << "SHFileOperation return code: " << result << std::endl;
        // Well, here is my attempt to use IFileOperation :p Fails with can't use MFC and windows.h
        // at the same time
        // HRESULT hr;
        // CFileFind finder;
        // std::string srcDir = user_folder_old;
        // std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        // std::wstring dstDir = converter.from_bytes(user_folder_new);

        // CComPtr<IFileOperation> fileOp;
        // fileOp.CoCreateInstance(CLSID_FileOperation);
        // fileOp->SetOperationFlags(FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI |
        // FOF_NOCONFIRMMKDIR);

        // srcDir += "\\*";
        // BOOL next = finder.FindFile(srcDir.c_str());
        // while (next) {
        //    next = finder.FindNextFile();
        //    if (finder.IsDots())
        //        continue;

        //    CComPtr<IShellItem> src, dst;
        //    CComBSTR bstr = finder.GetFilePath();
        //    if (S_OK != SHCreateItemFromParsingName(bstr, NULL, IID_PPV_ARGS(&src)))
        //        continue;

        //    if (S_OK != SHCreateItemFromParsingName(dstDir.c_str(), NULL, IID_PPV_ARGS(&dst)))
        //        continue;

        //    fileOp->CopyItem(src, dst, 0, NULL);
        //}

        // hr = fileOp->PerformOperations();
    }
    // if the old folder doesn't have a user directory, just ignore it.
    myfile << "ending copy of old user folder " << std::endl;
    myfile.close();
}

bool checkInternetConnection() {
    // Poor man's internet connection testing.
    return InternetCheckConnection("https://api.github.com", FLAG_ICC_FORCE_CONNECTION, 0);
}

std::string fetchLatestTag() {
    HINTERNET initialize, connection, file;
    DWORD dwBytes;
    char buf[100000];
    initialize = InternetOpen("Citrabot", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    connection = InternetConnect(initialize, "api.github.com", INTERNET_DEFAULT_HTTPS_PORT, NULL,
                                 NULL, INTERNET_SERVICE_HTTP, 0, 0);
    file = HttpOpenRequest(
        connection, "GET", "/repos/citra-emu/citra-bleeding-edge/releases/latest", "HTTP/1.1", NULL,
        NULL, INTERNET_FLAG_RELOAD | INTERNET_FLAG_EXISTING_CONNECT | INTERNET_FLAG_SECURE, 0);
    std::string output;
    if (HttpSendRequest(file, NULL, 0, NULL, 0)) {
        while (InternetReadFile(file, &buf, sizeof(buf), &dwBytes)) {
            if (dwBytes == 0) {
                break;
            }
            output += buf;
        }
    }

    InternetCloseHandle(file);
    InternetCloseHandle(connection);
    InternetCloseHandle(initialize);

    // fetch the tag name from the json
    // gcc seems to have spotty regex support still
    // std::string tag_name = "";
    // std::smatch match;
    // std::regex tag_regex(".*\"tag_name\":\\s*\"([^\"]*)\".*");
    // if (std::regex_search(output, match, tag_regex)) {
    //     tag_name = match[1];
    // }
    auto js = nlohmann::json::parse(output.c_str());
    return js["tag_name"];
}

int main(int argc, char** argv) {
    int option_index = 0;
    int argc_w;
    auto argv_w = CommandLineToArgvW(GetCommandLineW(), &argc_w);

    if (argv_w == nullptr) {
        std::cout << "Failed to get command line arguments" << std::endl;
        return -1;
    }

    static struct option long_options[] = {{"squirrel-firstrun", no_argument, 0, 'f'},
                                           {"squirrel-install", required_argument, 0, 'i'},
                                           {"squirrel-obsolete", required_argument, 0, 'o'},
                                           {"squirrel-updated", required_argument, 0, 'u'},
                                           {"squirrel-uninstall", required_argument, 0, 'r'},
                                           {0, 0, 0, 0}};

    // Get the path to the main applications
    char buffer[MAX_PATH];
    GetModuleFileName(NULL, buffer, MAX_PATH);
    std::string fullpath = std::string(buffer);
    std::string path = fullpath.substr(0, fullpath.find_last_of("\\/"));
    std::string msvc_redist_exe = path + "\\vcredist_x64.exe";
    std::string updater_exe = path.substr(0, path.find_last_of("\\/") + 1) + "Update.exe";

    while (optind < argc) {
        char arg = getopt_long(argc, argv, "fi:o:u:r:", long_options, &option_index);
        if (arg != -1) {
            switch (arg) {
            case 'f':
                // First time run. Just launch.
                displayWelcome();
                startProcessWrapper(updater_exe, "Update.exe --processStart=citra-qt.exe", false);
                return 0;
            case 'i':
                // On install. Do what is needed and exit fast.
                // TODO install msvc as part of the msi installer and leave it outta builds
                // installMSVCRuntime(msvc_redist_exe.c_str());
                startProcessWrapper(updater_exe, "Update.exe --createShortcut=updater.exe", true);
                return 0;
            case 'o':
                // Obsolete. Not sure what to use it for.
                return 0;
            case 'u':
                // Update: Find the old user directory and copy it over
                copyPreviousInstallsUserFolder(path);
                startProcessWrapper(updater_exe, "Update.exe --removeShortcut=updater.exe", true);
                startProcessWrapper(updater_exe, "Update.exe --createShortcut=updater.exe", true);
                startProcessWrapper(updater_exe, "Update.exe --processStart=citra-qt.exe", false);
                return 0;
            case 'r':
                // This throws an error when everything is somehow gone when it tries to remove the
                // shortcut...
                startProcessWrapper(updater_exe, "Update.exe --removeShortcut=updater.exe", true);
                return 0;
            }
        } else {
            optind++;
        }
    }
    // check for updates in the background
    // fetch the latest tag number from gith
    if (checkInternetConnection()) {
        std::string tag_name = fetchLatestTag();
        // if the version is the same, just start citra
        if (tag_name != "" && tag_name != Updater::tag_name) {
// Bring up the update dialog
#ifdef NO_TASKDIALOG
            // Mingw64 on windows on appveyor doesn't support TASKDIALOG grr
            int retval = MessageBoxW(NULL, L"An update is available. Would you like to update now?",
                                     L"Citra", MB_ICONEXCLAMATION | MB_YESNO);
            if (retval == IDYES) {
                startProcessWrapper(updater_exe, "Update.exe "
                                                 "--update=https://github.com/citra-emu/"
                                                 "citra-bleeding-edge/releases/download/" +
                                                     tag_name,
                                    true);
                return 0;
            }
#else
            HRESULT hr;
            TASKDIALOGCONFIG tdc = {sizeof(TASKDIALOGCONFIG)};
            int selection;
            // BOOL checkbox;
            std::wstring mbox_title = L"Citra";
            std::wstring mbox_header = L"An update to Citra is available!";
            std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
            std::wstring mbox_extra_info =
                converter.from_bytes("Current version: " + std::string(Updater::tag_name) +
                                     "\nLatest version: " + tag_name);
            // TODO: save this to a settings file like updater.ini
            // std::wstring mbox_checkbox_text = L"Don't ask me about updating again";
            TASKDIALOG_BUTTON buttons[] = {{1000, L"Update to the latest version"},
                                           {1001, L"Don't update right now"}};

            tdc.hwndParent = nullptr;
            tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS;
            tdc.pButtons = buttons;
            tdc.cButtons = _countof(buttons);
            tdc.nDefaultButton = 1000;
            tdc.pszWindowTitle = mbox_title.c_str();
            tdc.pszMainIcon = TD_INFORMATION_ICON;
            tdc.pszMainInstruction = mbox_header.c_str();
            tdc.pszExpandedInformation = mbox_extra_info.c_str();
            // tdc.pszVerificationText = mbox_checkbox_text.c_str();

            hr = TaskDialogIndirect(&tdc, &selection, nullptr, nullptr);
            if (selection == 1000) {
                // download the latest version and get it ready
                // TODO display some updater dialog... maybe the loading gif
                startProcessWrapper(updater_exe, "Update.exe "
                                                 "--update=https://github.com/citra-emu/"
                                                 "citra-bleeding-edge/releases/download/" +
                                                     tag_name,
                                    true);
                // and we are done! Next boot it'll point to the new version.
                return 0;
            }
#endif
        }
    }
    // Normal Run so just start citra-qt
    startProcessWrapper(updater_exe, "Update.exe --processStart=citra-qt.exe", false);
    return 0;
}
