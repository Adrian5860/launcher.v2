#pragma once
// =========================================================================
//  MultiDownloader.h
//  Header-only, wielowątkowy downloader oparty na libcurl (easy interface)
//  z paskiem postępu w konsoli w stylu PowerShellowego rozpakowywania .zip.
//
//  Wymagania:
//    - libcurl (nagłówki + link do libcurl.a / libcurl.dll.a / libcurl.lib)
//    - C++17 (std::thread, std::atomic, std::filesystem opcjonalnie)
//
//  Użycie (w pliku .cpp):
//
//      #include "MultiDownloader.h"
//
//      int main() {
//          MultiDownloader dl(4); // 4 wątki robocze na raz
//          dl.addDownload("https://example.com/plik1.zip", "plik1.zip");
//          dl.addDownload("https://example.com/plik2.zip", "plik2.zip");
//          dl.start(); // blokuje, rysuje pasek postępu, zwraca po zakończeniu
//      }
//
//  Link: -lcurl (mingw: -lcurl -lws2_32 -lwldap32 -lcrypt32 zależnie od buildu)
// =========================================================================

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Dostosuj tę ścieżkę do swojego zestawu curl-8.22.0_1-win64-mingw,
// albo po prostu dodaj katalog include curla do include path projektu
// i zostaw #include <curl/curl.h>.

#include "curl-8.22.0_1-win64-mingw\include\curl\curl.h"

// -------------------------------------------------------------------------
//  RAII init/cleanup dla globalnego stanu libcurl (curl_global_init/cleanup
//  trzeba wywołać dokładnie raz, zanim odpalimy jakiekolwiek wątki z curlem)
// -------------------------------------------------------------------------
class CurlGlobalGuard
{
public:
    CurlGlobalGuard()  { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobalGuard() { curl_global_cleanup(); }

    CurlGlobalGuard(const CurlGlobalGuard&)            = delete;
    CurlGlobalGuard& operator=(const CurlGlobalGuard&) = delete;
};

// -------------------------------------------------------------------------
//  Stan pojedynczego pobierania - aktualizowany na żywo przez wątek roboczy,
//  odczytywany przez wątek rysujący pasek postępu.
// -------------------------------------------------------------------------
struct DownloadItem
{
    std::string url;
    std::string outputPath;

    std::atomic<curl_off_t> downloadedBytes{0};
    std::atomic<curl_off_t> totalBytes{0};
    std::atomic<bool>       finished{false};
    std::atomic<bool>       failed{false};
    std::string             errorMessage; // ustawiane tylko po zakończeniu wątku roboczego
};

// -------------------------------------------------------------------------
//  Tworzy i konfiguruje jeden uchwyt CURL* z sensownymi domyślnymi opcjami
//  (redirecty, timeouty, SSL, User-Agent). Zwraca nullptr przy błędzie.
// -------------------------------------------------------------------------
inline CURL* make_curl()
{
    CURL* curl = curl_easy_init();
    if (!curl)
        return nullptr;

    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L); // brak globalnego limitu czasu transferu
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MultiDownloader/1.0 (libcurl)");
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L); // HTTP >= 400 => błąd

    return curl;
}

// -------------------------------------------------------------------------
//  Główna klasa: kolejka pobrań, pula wątków, pasek postępu.
// -------------------------------------------------------------------------
class MultiDownloader
{
public:
    explicit MultiDownloader(size_t maxThreads = 4)
        : maxThreads_(maxThreads == 0 ? 1 : maxThreads)
    {
    }

    // Dodaje plik do kolejki. Wywołuj przed start().
    void addDownload(const std::string& url, const std::string& outputPath)
    {
        auto item          = std::make_shared<DownloadItem>();
        item->url          = url;
        item->outputPath   = outputPath;
        items_.push_back(item);
    }

    // Czyści kolejkę pobierania.
    void clearQueue()
    {
        items_.clear();
    }

    // Blokuje wątek wywołujący, dopóki wszystkie pliki się nie pobiorą
    // (albo nie zawiodą). Rysuje pasek postępu w konsoli.
    // Zwraca true, jeśli WSZYSTKIE pliki pobrały się poprawnie.
    bool start()
    {
        if (items_.empty())
            return true;

        for (size_t i = 0; i < items_.size(); ++i)
            pending_.push(i);

        size_t threadCount = (std::min)(maxThreads_, items_.size());
        std::vector<std::thread> workers;
        workers.reserve(threadCount);

        std::atomic<bool> workersDone{false};
        std::thread progressThread(&MultiDownloader::progressLoop, this, std::ref(workersDone));

        for (size_t i = 0; i < threadCount; ++i)
            workers.emplace_back(&MultiDownloader::workerLoop, this);

        for (auto& t : workers)
            t.join();

        workersDone = true;
        progressThread.join();

        printFinalSummary();

        bool allOk = true;
        for (auto& it : items_)
            if (it->failed) allOk = false;

        return allOk;
    }

private:
    // ---- pula wątków -----------------------------------------------------

    void workerLoop()
    {
        while (true)
        {
            size_t index;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                if (pending_.empty())
                    return;
                index = pending_.front();
                pending_.pop();
            }
            downloadOne(items_[index]);
        }
    }

    void downloadOne(std::shared_ptr<DownloadItem> item)
    {
        FILE* file = std::fopen(item->outputPath.c_str(), "wb");
        if (!file)
        {
            item->failed       = true;
            item->errorMessage = "Nie mozna otworzyc pliku docelowego: " + item->outputPath;
            item->finished      = true;
            return;
        }

        CURL* curl = make_curl();
        if (!curl)
        {
            std::fclose(file);
            item->failed       = true;
            item->errorMessage = "curl_easy_init() zwrocilo null";
            item->finished      = true;
            return;
        }

        curl_easy_setopt(curl, CURLOPT_URL, item->url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &MultiDownloader::writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &MultiDownloader::progressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, item.get());
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

        CURLcode res = curl_easy_perform(curl);

        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_easy_cleanup(curl);
        std::fclose(file);

        if (res != CURLE_OK)
        {
            item->failed       = true;
            item->errorMessage = std::string("curl error: ") + curl_easy_strerror(res);
        }

        item->finished = true;
    }

    static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        FILE* file = static_cast<FILE*>(userdata);
        return std::fwrite(ptr, size, nmemb, file);
    }

    static int progressCallback(void* clientp,
                                 curl_off_t dltotal, curl_off_t dlnow,
                                 curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
    {
        auto* item = static_cast<DownloadItem*>(clientp);
        item->totalBytes      = dltotal;
        item->downloadedBytes = dlnow;
        return 0; // 0 = kontynuuj, cokolwiek innego = przerwij transfer
    }

    // ---- pasek postępu -----------------------------------------------------

    void progressLoop(std::atomic<bool>& workersDone)
    {
        using namespace std::chrono;

        auto lastTime  = steady_clock::now();
        curl_off_t lastBytes = 0;
        double speedBytesPerSec = 0.0;

        while (!workersDone)
        {
            renderProgress(lastTime, lastBytes, speedBytesPerSec);
            std::this_thread::sleep_for(milliseconds(150));
        }
        // ostatnie odświeżenie, żeby pasek pokazał 100%
        renderProgress(lastTime, lastBytes, speedBytesPerSec);
        std::cout << "\n";
    }

    void renderProgress(std::chrono::steady_clock::time_point& lastTime,
                         curl_off_t& lastBytes,
                         double& speedBytesPerSec)
    {
        curl_off_t downloaded = 0;
        curl_off_t total      = 0;
        size_t     doneCount  = 0;

        for (auto& item : items_)
        {
            downloaded += item->downloadedBytes.load();
            total      += item->totalBytes.load();
            if (item->finished) ++doneCount;
        }

        // Aktualizacja prędkości co ~tick
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - lastTime).count();
        if (dt >= 0.3)
        {
            speedBytesPerSec = (downloaded - lastBytes) / dt;
            lastBytes = downloaded;
            lastTime  = now;
        }

        double percent = 0.0;
        if (total > 0)
            percent = 100.0 * static_cast<double>(downloaded) / static_cast<double>(total);
        else if (doneCount == items_.size())
            percent = 100.0;

        const int barWidth = 30;
        int filled = static_cast<int>(percent / 100.0 * barWidth);
        if (filled > barWidth) filled = barWidth;
        if (filled < 0) filled = 0;

        std::ostringstream line;
        line << "\rPobieranie: [";
        for (int i = 0; i < barWidth; ++i)
            line << (i < filled ? '#' : '-');
        line << "] " << std::fixed << std::setprecision(1) << std::setw(5) << percent << "%  "
             << "(" << doneCount << "/" << items_.size() << " plikow)  "
             << formatBytes(downloaded) << " / " << (total > 0 ? formatBytes(total) : std::string("?"))
             << "  " << formatSpeed(speedBytesPerSec) << "   ";

        std::cout << line.str() << std::flush;
    }

    void printFinalSummary()
    {
        std::cout << "\nZakonczono pobieranie " << items_.size() << " plikow:\n";
        for (auto& item : items_)
        {
            std::cout << "  - " << item->outputPath << ": "
                      << (item->failed ? ("BLAD (" + item->errorMessage + ")") : std::string("OK"))
                      << "\n";
        }
    }

    static std::string formatBytes(curl_off_t bytes)
    {
        double b = static_cast<double>(bytes);
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        int u = 0;
        while (b >= 1024.0 && u < 4)
        {
            b /= 1024.0;
            ++u;
        }
        std::ostringstream out;
        out << std::fixed << std::setprecision(u == 0 ? 0 : 1) << b << " " << units[u];
        return out.str();
    }

    static std::string formatSpeed(double bytesPerSec)
    {
        if (bytesPerSec < 0) bytesPerSec = 0;
        return formatBytes(static_cast<curl_off_t>(bytesPerSec)) + "/s";
    }

    size_t maxThreads_;
    std::vector<std::shared_ptr<DownloadItem>> items_;
    std::queue<size_t> pending_;
    std::mutex queueMutex_;

    // Trzyma globalną inicjalizację curla żywą przez cały czas życia obiektu.
    CurlGlobalGuard globalGuard_;
};
