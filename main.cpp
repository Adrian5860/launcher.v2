#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include "miniz-3.1.2/miniz.h"
#include "MultiDownloader.h"

namespace fs = std::filesystem;

#ifdef _WIN32
	#include <windows.h>

	void set_utf8_console()
	{
	    SetConsoleOutputCP(CP_UTF8);
	    SetConsoleCP(CP_UTF8);
	}
#else
	#include <unistd.h>
	#include <sys/wait.h>
	
	#include <locale.h>
	void set_utf8_console()
	{
	    setlocale(LC_ALL, "");
	}
#endif

const std::string is_active_link = "https://raw.githubusercontent.com/Adrian5860/launcher.v2/main/preview.txt";
const std::string pinecone_link  = "https://github.com/ElyPrismLauncher/Launcher/releases/download/11.1.0/PineconeMC-Windows-MSVC-Portable-11.1.0.zip";
const std::string modpack_link   = "https://raw.githubusercontent.com/Adrian5860/launcher.v2/main/ligma_modpack.zip";
std::string Username = "";
bool is_setup        = false;
char eastereggs      = '0';
char input;
/** 0 - no egg
*  1 - failed to choose 1, 2 or 3.
*/

MultiDownloader dl(1);

// -------------------------------------------------------------------------
// Czysci wejscie uzytkownika aby pozbyc sie nieprzewidzianych rzeczy
// -------------------------------------------------------------------------
void clear_input()
{
    std::cin.clear();
    std::string dummy;
    std::getline(std::cin, dummy); // wyrzuca reszte biezacej linii
}

// -------------------------------------------------------------------------
//  Uruchamia zewnetrzny program. `args` to lista osobnych argumentow,
//  zeby uniknac problemow z cytowaniem sciezek zawierajacych spacje.
//  Zaklada wzgledne sciezki bez polskich znakow.
// -------------------------------------------------------------------------
bool create_process(const std::string& exePath,
                     const std::vector<std::string>& args,
                     const fs::path& waitForFile = {},   // jeśli podane: czekaj aż plik się pojawi, potem zabij
                     int waitForFileTimeoutMs = 30000)    // max czas oczekiwania w ms
{
#ifdef _WIN32
    std::ostringstream cmd;
    cmd << "\"" << exePath << "\"";
    for (auto& a : args)
        cmd << " \"" << a << "\"";

    std::string cmdLine = cmd.str();
    std::vector<char> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr,
                              FALSE, 0, nullptr, ".pinecone", &si, &pi);
    if (!ok)
    {
        std::cerr << "Nie udalo sie uruchomic: " << exePath
                   << " (kod bledu: " << GetLastError() << ")\n";
        return false;
    }

    if (!waitForFile.empty())
    {
        std::cout << "Czekam, az launcher pobierze pakiet jezykowy...\n";

        auto start = std::chrono::steady_clock::now();
        while (!fs::exists(waitForFile))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (elapsed > waitForFileTimeoutMs)
            {
                std::cerr << "Timeout: folder jezykowy sie nie pojawil, przerywam czekanie.\n";
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // daj chwilę na dopisanie pliku
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, INFINITE);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;

#else
    pid_t pid = fork();
    if (pid < 0)
    {
        std::cerr << "fork() nie powiodl sie\n";
        return false;
    }

    if (pid == 0)
    {
        // dziecko: odlacz od terminala/sesji rodzica
        setsid();

        // drugi fork - zeby wnuk zostal "sierota" przejeta przez init,
        // a nie pozostal zwiazany z (juz konczacym sie) pierwszym dzieckiem
        pid_t pid2 = fork();
        if (pid2 < 0)
            std::exit(1);

        if (pid2 > 0)
            std::exit(0); // pierwsze dziecko konczy sie natychmiast

        // tutaj jestesmy juz we wnuku - odlaczonym, niezaleznym procesie
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(exePath.c_str()));
        for (auto& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        execv(exePath.c_str(), argv.data());
        std::exit(127); // execv wraca tylko przy bledzie
    }

    // rodzic: odbiera pierwsze dziecko (ktore zaraz samo sie konczy),
    // zeby nie zostawic zombie
    int status = 0;
    waitpid(pid, &status, 0);

    return true;
#endif
}

// -------------------------------------------------------------------------
//  Rozpakowuje archiwum .zip do podanego katalogu, za pomocą biblioteki
//  miniz, która działa na windows i linux
// -------------------------------------------------------------------------
bool extract_archive(const std::string& archivePath, const std::string& destDir)
{
    mz_zip_archive zip{};

    if (!mz_zip_reader_init_file(&zip, archivePath.c_str(), 0))
    {
        std::cerr << "Nie udalo sie otworzyc archiwum: " << archivePath << "\n";
        return false;
    }

    if (!fs::exists(destDir))
        fs::create_directories(destDir);

    mz_uint fileCount = mz_zip_reader_get_num_files(&zip);
    std::cout << "Rozpakowywanie " << archivePath << " (" << fileCount << " plikow)...\n";

    fs::path canonicalDest = fs::weakly_canonical(destDir); // <-- policz RAZ, przed petla

    for (mz_uint i = 0; i < fileCount; ++i)
    {
        mz_zip_archive_file_stat fileStat;
        if (!mz_zip_reader_file_stat(&zip, i, &fileStat))
        {
            std::cerr << "Nie udalo sie odczytac metadanych pliku #" << i << "\n";
            mz_zip_reader_end(&zip);
            return false;
        }

        fs::path outPath = fs::path(destDir) / fileStat.m_filename;

        // --- sprawdzenie Zip Slip: TU, zaraz po zbudowaniu outPath ---
        fs::path canonicalOut = fs::weakly_canonical(outPath);
        if (canonicalOut.string().find(canonicalDest.string()) != 0)
        {
            std::cerr << "Podejrzana sciezka w archiwum, pomijam: " << fileStat.m_filename << "\n";
            continue;
        }
        // --- koniec sprawdzenia ---

        if (mz_zip_reader_is_file_a_directory(&zip, i))
        {
            fs::create_directories(outPath);
            continue;
        }

        fs::create_directories(outPath.parent_path());

        if (!mz_zip_reader_extract_to_file(&zip, i, outPath.string().c_str(), 0))
        {
            std::cerr << "Nie udalo sie wyodrebnic: " << fileStat.m_filename << "\n";
            mz_zip_reader_end(&zip);
            return false;
        }
    }

    mz_zip_reader_end(&zip);
    std::cout << "Rozpakowano pomyslnie.\n";
    return true;
}

// -------------------------------------------------------------------------
// pobiera modpack, czyli archiwum .zip zawierające mody itd.
// -------------------------------------------------------------------------
// true - rozpakowuje i usuwa.zip
// false - zostawia .zip
void import_modpack(fs::path modpackPath, const bool& extract)
{
    if(fs::exists(modpackPath.generic_string() + "/minecraft")) //???
    {
        if(fs::exists("ligma_modpack.zip")) fs::remove("ligma_modpack.zip");

        return;
    }
    else
    {
        if(!extract)
        {
            fs::create_directories(modpackPath);
        }
    }
    std::cout << "Pobieranie paczki modów...\n";

    if(!fs::exists("ligma_modpack.zip"))
    {
        std::cout << "Pobieranie...\n";
        
        dl.addDownload(modpack_link, "ligma_modpack.zip");
        if (!dl.start())
        {
            std::cerr << "Pobieranie nie powiodlo sie, przerywam.\n";
            return;
        }
        
        dl.clearQueue();
    }
    else
    {
        std::cout << "ligma_modpack.zip jest już pobrany.";
    }

    if(!extract)
    {
        return;
    }

    if(extract_archive("ligma_modpack.zip", modpackPath.generic_string()))
    {
        fs::remove("ligma_modpack.zip");
    }
}

// -------------------------------------------------------------------------
// Sprawdza, czy jest już pobrany launcher i go pobiera
// -------------------------------------------------------------------------
void check_installation()
{
    if(!fs::exists(".pinecone"))
    {
        fs::create_directory(".pinecone");
        // SetFileAttributesA(".pinecone", FILE_ATTRIBUTE_HIDDEN);
    }

    if(fs::exists(".pinecone/elyprismlauncher.exe"))
    {
        std::cout << "PineconeMC jest już zainstalowany.\n";
        
        if(fs::exists("PineconeMC.zip")) fs::remove("PineconeMC.zip");

        return;
    }

    std::cout << "Nie znaleziono zainstalowanego launchera.\n";

    if(!fs::exists("PineconeMC.zip"))
    {
        std::cout << "Pobieranie...\n";
        
        dl.addDownload(pinecone_link, "PineconeMC.zip");
        if (!dl.start())
        {
            std::cerr << "Pobieranie nie powiodlo sie, przerywam.\n";
            return;
        }

        dl.clearQueue();
    }
    else
    {
        std::cout << "PineconeMC.zip jest już pobrany.";
    }

    if(extract_archive("PineconeMC.zip", ".pinecone"))
    {
        fs::remove("PineconeMC.zip");
    }
}

// zapisuje accounts.json
bool save_file_accounts_json(const fs::path& filepath)
{
    std::ofstream f(filepath, std::ios::binary);
    if (!f.is_open())
        return false;

    f << "{\n"
      << "    \"accounts\": [\n"
      << "        {\n"
      << "            \"active\": true,\n"
      << "            \"profile\": {\n"
      << "                \"capes\": [\n"
      << "                ],\n"
      << "                \"id\": \"00000000000000000000000000000000\",\n"
      << "                \"name\": \"" << Username << "\",\n"
      << "                \"skin\": {\n"
      << "                    \"id\": \"\",\n"
      << "                    \"url\": \"\",\n"
      << "                    \"variant\": \"\"\n"
      << "                }\n"
      << "            },\n"
      << "            \"type\": \"Offline\",\n"
      << "            \"ygg\": {\n"
      << "                \"extra\": {\n"
      << "                    \"clientToken\": \"00000000000000000000000000000000\",\n"
      << "                    \"userName\": \"" << Username << "\"\n"
      << "                },\n"
      << "                \"iat\": 1,\n"
      << "                \"token\": \"0\"\n"
      << "            }\n"
      << "        }\n"
      << "    ],\n"
      << "    \"formatVersion\": 3\n"
      << "}\n";

    return f.good();
}

// zapisuje elyprismlauncher.cfg
bool SaveElyprismlauncherCfg(const fs::path& filepath)
{
    if (fs::exists(filepath))
    {
        return true;
    }
    std::ofstream f(filepath, std::ios::binary);
    if (!f.is_open())
    {
        return false;
    }

    f << "[General]\n"
      << "ConfigVersion=1.3\n"
      << "ApplicationTheme=dark\n"
      << "IconTheme=pe_colored\n"
      << "Language=pl\n"
      << "CloseAfterLaunch=true\n"
      << "DownloadsDir=downloads\n"
      << "ElyPatchPreference=2\n"
      << "QuitAfterGameStop=true\n";

    return f.good();
}

// -------------------------------------------------------------------------
// dodaje pliki, aby pominąć ręczny setup użytkownika
// -------------------------------------------------------------------------
// accounts.json 
// elyprismlauncher.cfg
// MultiMC_nomigrate.txt
// PolyMC_nomigrate.txt
// Prism Launcher_nomigrate.txt
bool patch_it()
{
    // create *_nomigrate.txt files
    auto create_if_missing = [](const fs::path& p) -> bool {
        if (fs::exists(p))
        {
            return true; 
        }

        std::ofstream f(p, std::ios::out);
        if (!f.is_open())
        {
            return false;
        }
        f.close();
        return true;
    };

    if(
        create_if_missing(".pinecone/MultiMC_nomigrate.txt") &&
        create_if_missing(".pinecone/PolyMC_nomigrate.txt") && 
        create_if_missing(".pinecone/Prism Launcher_nomigrate.txt") &&
        save_file_accounts_json(".pinecone/accounts.json") &&
        SaveElyprismlauncherCfg(".pinecone/elyprismlauncher.cfg")
    )
    {
        return true;
    }
    
    return false;
}

bool save_choices_cfg(const fs::path& file)
{
    std::ofstream c(file, std::ios::binary);
    if (!c.is_open())
    {
        std::cerr << "Nie udało się otworzyć " << file << '\n';
        return false;
    }

    c << "Input=" << input << '\n';
    c << "EasterEggs=" << eastereggs << '\n';
    c << "Username=" << Username << '\n';
    c << "Setup=" << (is_setup ? '1' : '0') << '\n';

    return c.good();
}

// -------------------------------------------------------------------------
//  Uruchamia funkcje przygotowujące i gotowy launcher 
// -------------------------------------------------------------------------
// true - sam launcher
// false - launcher, modpack itd.
void launch_game(const bool& no_args)
{
    check_installation();
    if(!patch_it())
    {
        std::cerr << "Nie udało się zastosować 'poprawek'.\n";
    }
    import_modpack(".pinecone/instances/ligma_modpack", true);
    
    if (!is_setup)
    {
        create_process(".pinecone\\elyprismlauncher.exe", {}, ".pinecone/translations/index_v2.json");

        is_setup = true;
        save_choices_cfg("choices.cfg");
    }

    if(no_args)
    {
        create_process(".pinecone\\elyprismlauncher.exe", {});
    }
    else
    {
        create_process(".pinecone\\elyprismlauncher.exe", {"-lligma_modpack", "-a" + Username});
    }
    std::exit(0);
}

void setup(fs::path file)
{
    if(fs::exists(file))
    {
        // załaduj choices.cfg
        std::ifstream c(file, std::ios::binary);
        if (!c.is_open())
        {
            std::cerr << "Nie udało się otworzyć " << file << '\n';
        }

        std::string line;
        while (std::getline(c, line))
        {
            auto pos = line.find('=');
            if (pos == std::string::npos)
            {
                continue;
            }

            std::string key   = line.substr(0, pos);
            std::string value = line.substr(pos + 1);

            if (key == "Input")
            {
                input = std::stoi(value) + '0';
            }
            else if (key == "EasterEggs")
            {
                eastereggs = std::stoi(value) + '0';
            }
            else if (key == "Username")
            {
                Username = value;
            }
            else if (key == "Setup")
            {
                is_setup = std::stoi(value);
            }
        }
        c.close();
    }
    else
    {   
        if(!fs::exists(".pinecone/accounts.json") || Username == "")
        {
            std::cout << "Podaj nazwę użytkownika - taki nick będziesz mieć w grze.\nWpisanie nieprawidłowej nazwy spowoduje to, że launcher się nie uruchomi.\nNaciśnij 'Enter' aby zatwierdzić...\n\n";
            std::cin >> Username;
            clear_input();
        }
        std::cout << "\nWitaj " << Username << "!                                                                   (autor: Adrian5860)\n\nTen program zainstaluje launcher w folderze, w którym uruchamiasz ten plik.\n";
        std::cout << "Jeżeli chcesz zainstalować go w innym folderze, zamknij ten plik i przenieś w odpowiednie dla Ciebie miejsce.\nWszystko związane z tym programem będzie w miarę możliwości znajdowało się w tym folderze.\n\n";
        std::cout << "Jeżeli:\n1) chcesz mieć już to z głowy i nie chcesz dodawać własnych modów - wpisz 1; (zalecane)\n2) chcesz dodawać własne mody i zmienić ustawienia launchera - wpisz 2;\n";
        std::cout << "3) chcesz pobrać tylko mody wymagane na serwerze, bo używasz własnego launchera :( - wpisz 3.\nJeżeli będziesz chcieć w przyszłości zmienić swój wybór, usuń plik 'choices.cfg'.\n";
        std::cout << "Jeżeli program nie zadziała w wybrany sposób, zgłoś to, ja postaram się pomóc.\n\nPo wpisaniu odpowiedniej liczby naciśnij 'Enter'...\n";

        while(true)
        {
            std::cin >> input;
            clear_input();

            if(input == '1' || input == '2' || input == '3')
            {
                break;
            }

            if (eastereggs == '0')
            {
                eastereggs = '1';
                std::cout << "Poważnie? Masz do wpisania 1, 2 lub 3, a wpisujesz co innego? Popraw się. (Twoje wybory będą miały konsekwencje...)\n";
            }
            else
            {
                std::cout << "Nie uczysz się na błędach co? Wybierz jedną z dostępnych opcji poprzez wpisanie odpowiedniej liczby i wciśnij 'Enter' ...\n";
            }
        }
        save_choices_cfg(file);
    }
    switch(input)
    {  
        case '1':
            launch_game(false);
            break;
        case '2':
            launch_game(true);
            break;
        case '3':
            import_modpack("ligma_modpack", false);
            std::cout << "Możesz importować całą paczkę jako .zip, lub rozpakować archiwum i skopiować mody z folderu './minecraft/mods'.\n";
            std::this_thread::sleep_for(std::chrono::seconds(5));
            break;
        default: 
            std::cerr << "jeżeli widzisz tą linijkę tekstu to znaczy że zepsułeś config.cfg\nI wiesz co? też cię lubię :)\n";
            fs::rename("choices.cfg", "po_co_ci_to_bylo--usun_to.cfg");
    }
}

// callback: dopisuje pobrane dane do std::string wskazywanego przez userdata
static size_t writeToString(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    std::string* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// usuwa białe znaki z początku i końca stringa
static std::string trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end   = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos)
        return "";
    return s.substr(start, end - start + 1);
}

// sprawdza is_active.txt pod is_active_link i zwraca jego zawartość jako bool
bool is_active()
{
    CURL* curl = make_curl();
    if (!curl)
    {
        std::cerr << "Nie udalo sie zainicjalizowac curl.\n";
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L); // nadpisuje 0L z make_curl()

    std::string content;
    curl_easy_setopt(curl, CURLOPT_URL, is_active_link.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &content);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        std::cerr << "Nie udalo sie pobrac is_active.txt: " << curl_easy_strerror(res) << "\n";
        return false; // brak dostepu = traktujemy jako "nieaktywne"
    }

    std::string trimmed = trim(content);
    return trimmed == "true";
}

int main()
{
    set_utf8_console();
    if(!is_active())
    {
        std::cout << "Przepraszam, ale program został wyłączony z działania.\nMoże to być zaplanowana przerwa techniczna, albo skończył się dostęp do serwera.\nDla pewności zawsze można zapytać na dc\n";
        std::cout << "Jeżeli nie ma informacji na discordzie o przerwie technicznej, prawdopodobnie program został wyłączony na stałe.\nMożesz usunąć całą zawartość folderu, w którym znajduje się ten plik.";
        std::cout << "\nTo okno zamknie się automatycznie za 30 sekund.\n\nDo zobaczenia :)";
        std::this_thread::sleep_for(std::chrono::seconds(30));
        std::exit(0);
    }
    setup("choices.cfg");
    std::cout << "Dzięki za skorzystanie z programu, do zobaczenia :)";
    std::this_thread::sleep_for(std::chrono::seconds(5));
    return 0;
}
