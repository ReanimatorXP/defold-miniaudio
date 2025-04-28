// ============================================================================
// Расширение для Defold для воспроизведения звуков (WAV, MP3) из локальной папки
// с использованием miniaudio. Включает управление громкостью/тоном,
// условное логирование и общую громкость.
// ============================================================================

#define WIN32_LEAN_AND_MEAN

// 1. SDK Defold
#include <dmsdk/sdk.h>

// 2. Настройка декодеров miniaudio
#define DR_MP3_IMPLEMENTATION       // Включаем реализацию MP3 декодера (dr_mp3)
#define MINIAUDIO_IMPLEMENTATION    // Основная реализация miniaudio (Должна идти ПОСЛЕ макросов конфигурации)

// 3. Отключение неиспользуемых форматов miniaudio
#define MA_NO_FLAC
#define MA_NO_WEBAUDIO
#define MA_NO_JACK
#define MA_NO_NULL

// 4. Включение бекендов Windows (miniaudio автоопределит для других платформ)
#define MA_ENABLE_WASAPI
#define MA_ENABLE_DSOUND
#define MA_ENABLE_WINMM

#include "miniaudio.h" // Включаем miniaudio ПОСЛЕ всех #define

// 5. Стандартные заголовки C++
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdio> // Для fopen/fclose
#include <algorithm> // Для std::max

// --- Структура для хранения информации о проигрываемом звуке ---
struct PlayingSound {
    ma_sound sound;         // Экземпляр звука miniaudio
    std::string name;       // Имя звука (для поиска и остановки)
    bool preloaded_instance;// true, если звук был создан из предзагруженного
};

// --- Глобальные переменные ---
static ma_engine g_maEngine;            // Глобальный движок miniaudio
static std::string g_basePath;          // Базовый путь к папке со звуками
static std::map<std::string, ma_sound> g_preloadedSounds; // Контейнер для предзагруженных звуков
static std::vector<PlayingSound*> g_playingSounds;      // Контейнер для активно играющих звуков
static std::mutex g_mutex;               // Мьютекс для защиты доступа к глобальным контейнерам

// Глобальный флаг для включения/выключения отладочных логов
// Управляется через miniaudio.set_debug(bool) из Lua. По умолчанию выключено.
static bool g_Miniaudio_IsDebugEnabled = false;

// Вспомогательный макрос для вывода отладочных сообщений dmLogInfo
// Выводит сообщение, только если g_Miniaudio_IsDebugEnabled == true
#define LOG_DEBUG(...) do { if (g_Miniaudio_IsDebugEnabled) dmLogInfo(__VA_ARGS__); } while(0)

// --- Вспомогательные функции ---

// Остановка и очистка всех активно играющих звуков
static void CleanupAllPlayingSounds() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (PlayingSound* playing : g_playingSounds) {
        ma_sound_stop(&playing->sound);
        ma_sound_uninit(&playing->sound); // Освобождаем ресурсы звука miniaudio
        delete playing;                   // Освобождаем память структуры PlayingSound
    }
    g_playingSounds.clear();
}

// Поиск файла звука по имени. Проверяет расширения .wav и .mp3.
// Возвращает полный путь к файлу или пустую строку, если файл не найден.
static std::string FindSoundFile(const std::string& soundName) {
    std::string pathWav = g_basePath + soundName + ".wav";
    std::string pathMp3 = g_basePath + soundName + ".mp3";

    LOG_DEBUG("FindSoundFile: Attempting WAV path: [%s]", pathWav.c_str());
    LOG_DEBUG("FindSoundFile: Attempting MP3 path: [%s]", pathMp3.c_str());

    FILE* fileWav = fopen(pathWav.c_str(), "rb");
    if (fileWav) {
        fclose(fileWav);
        LOG_DEBUG("FindSoundFile: Found WAV at %s", pathWav.c_str());
        return pathWav;
    }

    FILE* fileMp3 = fopen(pathMp3.c_str(), "rb");
    if (fileMp3) {
        fclose(fileMp3);
        LOG_DEBUG("FindSoundFile: Found MP3 at %s", pathMp3.c_str());
        return pathMp3;
    }

    dmLogWarning("FindSoundFile: Sound '%s' not found as .wav or .mp3 in base path '%s'", soundName.c_str(), g_basePath.c_str());
    return "";
}

// --- Lua API ---

// Lua: miniaudio.set_debug(enable)
// Включает (true) или выключает (false) вывод отладочных логов расширения.
static int Miniaudio_SetDebug(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN); // Убеждаемся, что аргумент - boolean
    bool enable = lua_toboolean(L, 1);
    g_Miniaudio_IsDebugEnabled = enable;
    dmLogInfo("miniaudio debug logging %s", enable ? "ENABLED" : "DISABLED"); // Сообщаем об изменении статуса
    return 0; // Не возвращаем значений в Lua
}

// Lua: miniaudio.set_base_path(path)
// Устанавливает базовый путь для поиска звуковых файлов.
static int Miniaudio_SetBasePath(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_basePath = path;
    // Добавляем слеш в конец пути, если его нет
    if (!g_basePath.empty() && g_basePath.back() != '/' && g_basePath.back() != '\\') {
        g_basePath += '/';
    }
    LOG_DEBUG("Miniaudio base path set to: %s", g_basePath.c_str());
    return 0;
}

// Lua: miniaudio.set_master_volume(volume)
// Устанавливает общую громкость для всего движка miniaudio.
// volume (number): Громкость от 0.0 (тишина) до N (1.0 = нормальная).
static int Miniaudio_SetMasterVolume(lua_State* L) {
    // Получаем громкость из Lua, проверяем, что это число
    double volume_double = luaL_checknumber(L, 1);
    float volume = (float)volume_double;

    // Ограничиваем громкость снизу (не может быть отрицательной)
    volume = std::max(0.0f, volume);

    // Устанавливаем громкость движка
    ma_result result = ma_engine_set_volume(&g_maEngine, volume);
    if (result != MA_SUCCESS) {
        // Логируем ошибку, если не удалось установить громкость
        dmLogError("Failed to set master volume: %s", ma_result_description(result));
    } else {
        LOG_DEBUG("Master volume set to: %.2f", volume);
    }

    return 0; // Не возвращаем значений в Lua
}

// Lua: miniaudio.preload(sound_name)
// Предзагружает звук (полностью декодирует в память).
static int Miniaudio_Preload(lua_State* L) {
    const char* soundName = luaL_checkstring(L, 1);
    std::string nameStr = soundName;
    ma_result result;

    std::lock_guard<std::mutex> lock(g_mutex);

    // Проверка, не предзагружен ли уже звук
    if (g_preloadedSounds.count(nameStr)) {
        dmLogWarning("Sound '%s' is already preloaded.", nameStr.c_str());
        lua_pushboolean(L, true);
        return 1;
    }

    // Ищем файл (WAV или MP3)
    std::string filePath = FindSoundFile(nameStr);
    if (filePath.empty()) {
        lua_pushboolean(L, false);
        return 1;
    }

    LOG_DEBUG("Preloading sound '%s' from: %s", nameStr.c_str(), filePath.c_str());

    // Проверка доступности файла непосредственно перед вызовом miniaudio
    FILE* f = fopen(filePath.c_str(), "rb");
    if (!f) {
         dmLogError("Failed to re-open file '%s' just before miniaudio init. Permissions?", filePath.c_str());
         lua_pushboolean(L, false);
         return 1;
    }
    fclose(f);

    // Получаем место в карте для хранения предзагруженного звука
    ma_sound* sound = &g_preloadedSounds[nameStr];
    ma_sound_config soundConfig = ma_sound_config_init();
    soundConfig.flags = MA_SOUND_FLAG_DECODE; // Флаг для полного декодирования
    soundConfig.pFilePath = filePath.c_str();

    LOG_DEBUG("Calling ma_sound_init_ex for preload with flags: %d", soundConfig.flags);
    result = ma_sound_init_ex(&g_maEngine, &soundConfig, sound);

    if (result != MA_SUCCESS) {
        g_preloadedSounds.erase(nameStr); // Удаляем неудачную запись из карты
        dmLogError("Failed to preload sound '%s' from '%s': %s (Error code: %d)",
                   nameStr.c_str(),
                   filePath.c_str(),
                   ma_result_description(result),
                   result);
        lua_pushboolean(L, false);
        return 1;
    }

    LOG_DEBUG("Sound '%s' preloaded successfully.", nameStr.c_str());
    lua_pushboolean(L, true);
    return 1;
}

// Lua: miniaudio.play(sound_name, [looping], [volume], [pitch])
// Воспроизводит звук.
// looping (boolean, опционально): Зациклить воспроизведение (по умолч. false).
// volume (number, опционально): Громкость от 0.0 до N (1.0 = нормальная, по умолч. 1.0).
// pitch (number, опционально): Высота тона/скорость (1.0 = нормальная, по умолч. 1.0).
static int Miniaudio_Play(lua_State* L) {
    int top = lua_gettop(L); // Количество аргументов

    // Получение аргументов из Lua
    const char* soundName = luaL_checkstring(L, 1);
    bool looping = (top >= 2) ? lua_toboolean(L, 2) : false;
    float volume = (float)luaL_optnumber(L, 3, 1.0); // Используем optnumber для значений по умолчанию
    float pitch = (float)luaL_optnumber(L, 4, 1.0);

    std::string nameStr = soundName;
    ma_result result;
    // Создаем структуру для хранения информации об этом экземпляре звука
    PlayingSound* playingSound = new PlayingSound();
    playingSound->name = nameStr;
    playingSound->preloaded_instance = false;

    std::lock_guard<std::mutex> lock(g_mutex); // Защищаем доступ к общим данным

    // Проверяем, был ли звук предзагружен
    auto preloadIt = g_preloadedSounds.find(nameStr);
    if (preloadIt != g_preloadedSounds.end()) {
        // Звук предзагружен - копируем его
        LOG_DEBUG("Playing preloaded sound: %s", nameStr.c_str());
        playingSound->preloaded_instance = true;
        // ma_sound_init_copy создает новый экземпляр звука на основе предзагруженного
        result = ma_sound_init_copy(&g_maEngine, &preloadIt->second, 0, NULL, &playingSound->sound);
         if (result != MA_SUCCESS) {
             delete playingSound;
             dmLogError("Failed to init copy for preloaded sound '%s': %s", nameStr.c_str(), ma_result_description(result));
             lua_pushboolean(L, false);
             return 1;
         }
    } else {
        // Звук не был предзагружен - загружаем с диска
        std::string filePath = FindSoundFile(nameStr);
        if (filePath.empty()) {
            delete playingSound;
            lua_pushboolean(L, false);
            return 1;
        }

        LOG_DEBUG("Playing sound '%s' directly from file: %s", nameStr.c_str(), filePath.c_str());

        // Дополнительная проверка доступности файла
        FILE* f = fopen(filePath.c_str(), "rb");
        if (!f) {
             dmLogError("Failed to re-open file '%s' just before miniaudio init. Permissions?", filePath.c_str());
             delete playingSound;
             lua_pushboolean(L, false);
             return 1;
        }
        fclose(f);

        // Инициализация звука из файла. Флаг 0 = MA_SOUND_FLAG_DECODE (полное декодирование).
        LOG_DEBUG("Calling ma_sound_init_from_file with flags: 0");
        result = ma_sound_init_from_file(&g_maEngine, filePath.c_str(), 0, NULL, NULL, &playingSound->sound);

        if (result != MA_SUCCESS) {
            delete playingSound;
            dmLogError("Failed to initialize sound '%s' from file '%s': %s (Error code: %d)",
                       nameStr.c_str(),
                       filePath.c_str(),
                       ma_result_description(result),
                       result);
            lua_pushboolean(L, false);
            return 1;
        }
    }

    // Применяем параметры (зацикливание, громкость, высота тона) к созданному звуку
    ma_sound_set_looping(&playingSound->sound, looping ? MA_TRUE : MA_FALSE);
    ma_sound_set_volume(&playingSound->sound, volume);
    ma_sound_set_pitch(&playingSound->sound, pitch);

    // Запускаем воспроизведение
    result = ma_sound_start(&playingSound->sound);

    if (result != MA_SUCCESS) {
        ma_sound_uninit(&playingSound->sound); // Очищаем ресурсы звука miniaudio
        delete playingSound;                   // Очищаем память структуры
        dmLogError("Failed to start sound '%s': %s", nameStr.c_str(), ma_result_description(result));
        lua_pushboolean(L, false);
        return 1;
    }

    // Добавляем успешно запущенный звук в список играющих
    g_playingSounds.push_back(playingSound);
    LOG_DEBUG("Started sound '%s' (Loop: %s, Vol: %.2f, Pitch: %.2f, Preloaded: %s)",
              nameStr.c_str(),
              looping ? "true" : "false",
              volume,
              pitch,
              playingSound->preloaded_instance ? "true" : "false");
    lua_pushboolean(L, true); // Сигнализируем Lua об успехе
    return 1;
}

// Lua: miniaudio.stop(sound_name)
// Останавливает все играющие экземпляры звука с указанным именем.
static int Miniaudio_Stop(lua_State* L) {
    const char* soundName = luaL_checkstring(L, 1);
    std::string nameStr = soundName;
    int stopped = 0; // Счетчик остановленных звуков

    std::lock_guard<std::mutex> lock(g_mutex);

    // Итерируем с конца, так как можем удалять элементы из вектора
    for (int i = g_playingSounds.size() - 1; i >= 0; --i) {
        PlayingSound* playing = g_playingSounds[i];
        if (playing->name == nameStr) {
            LOG_DEBUG("Stopping sound '%s'", nameStr.c_str());
            ma_sound_stop(&playing->sound);
            ma_sound_uninit(&playing->sound);
            delete playing;
            g_playingSounds.erase(g_playingSounds.begin() + i);
            stopped++;
        }
    }

    if (stopped == 0) {
        dmLogWarning("Stop: Sound '%s' not found playing.", nameStr.c_str());
    }
    lua_pushinteger(L, stopped); // Возвращаем количество остановленных звуков
    return 1;
}

// Lua: miniaudio.stop_all()
// Останавливает все играющие звуки.
static int Miniaudio_StopAll(lua_State* L) {
    int count = 0;
    {
       std::lock_guard<std::mutex> lock(g_mutex);
       count = g_playingSounds.size(); // Запоминаем количество перед очисткой
    }
    CleanupAllPlayingSounds(); // Выполняем фактическую остановку и очистку
    LOG_DEBUG("Stopped all %d playing sounds.", count);
    lua_pushinteger(L, count); // Возвращаем количество остановленных звуков
    return 1;
}

// Lua: miniaudio.is_playing(sound_name)
// Проверяет, играет ли хотя бы один экземпляр звука с указанным именем.
static int Miniaudio_IsPlaying(lua_State* L) {
    const char* soundName = luaL_checkstring(L, 1);
    std::string nameStr = soundName;
    bool is_playing = false;

    std::lock_guard<std::mutex> lock(g_mutex);

    for (const PlayingSound* playing : g_playingSounds) {
        if (playing->name == nameStr) {
            // ma_sound_is_playing() проверяет текущий статус звука
            if (ma_sound_is_playing(&playing->sound)) {
                is_playing = true;
                break; // Достаточно найти один играющий
            }
        }
    }

    lua_pushboolean(L, is_playing);
    return 1;
}

// Lua: miniaudio.unload(sound_name)
// Выгружает предзагруженный звук из памяти.
static int Miniaudio_Unload(lua_State* L) {
    const char* soundName = luaL_checkstring(L, 1);
    std::string nameStr = soundName;

    std::lock_guard<std::mutex> lock(g_mutex);

    auto it = g_preloadedSounds.find(nameStr);
    if (it != g_preloadedSounds.end()) {
        LOG_DEBUG("Unloading preloaded sound '%s'", nameStr.c_str());
        ma_sound_uninit(&it->second); // Освобождаем ресурсы звука miniaudio
        g_preloadedSounds.erase(it);  // Удаляем из карты
        lua_pushboolean(L, true);
    } else {
        dmLogWarning("Unload: Sound '%s' was not preloaded.", nameStr.c_str());
        lua_pushboolean(L, false);
    }
    return 1;
}


// --- Регистрация функций для Lua ---
// Список функций, которые будут доступны в Lua под таблицей "miniaudio".
static const luaL_reg Module_methods[] = {
    {"set_base_path", Miniaudio_SetBasePath},
    {"preload", Miniaudio_Preload},
    {"unload", Miniaudio_Unload},
    {"play", Miniaudio_Play},
    {"stop", Miniaudio_Stop},
    {"stop_all", Miniaudio_StopAll},
    {"is_playing", Miniaudio_IsPlaying},
    {"set_debug", Miniaudio_SetDebug},               // Функция управления логированием
    {"set_master_volume", Miniaudio_SetMasterVolume}, // Функция управления общей громкостью
    {0, 0} // Завершающий маркер списка
};

// --- Функции жизненного цикла экстеншена ---

// Вызывается при инициализации расширения
static dmExtension::Result InitializeMiniaudio(dmExtension::Params* params) {
    // Регистрируем наши C++ функции для доступа из Lua
    luaL_register(params->m_L, "miniaudio", Module_methods);
    lua_pop(params->m_L, 1); // Убираем таблицу модуля со стека Lua

    // Инициализируем движок miniaudio
    ma_engine_config config = ma_engine_config_init();
    ma_result result = ma_engine_init(&config, &g_maEngine);
    if (result != MA_SUCCESS) {
        dmLogError("Failed to initialize miniaudio engine: %s", ma_result_description(result));
        return dmExtension::RESULT_INIT_ERROR; // Возвращаем ошибку, если движок не запустился
    }

    // Очищаем контейнеры и сбрасываем флаги
    g_playingSounds.clear();
    g_preloadedSounds.clear();
    g_basePath = "";
    g_Miniaudio_IsDebugEnabled = false; // Сбрасываем флаг логов при инициализации

    dmLogInfo("Miniaudio Initialized. Miniaudio engine started (Sample Rate: %d, Channels: %d)",
              ma_engine_get_sample_rate(&g_maEngine),
              ma_engine_get_channels(&g_maEngine));
    return dmExtension::RESULT_OK;
}

// Вызывается каждый кадр (Update)
static dmExtension::Result UpdateMiniaudio(dmExtension::Params* params) {
    std::lock_guard<std::mutex> lock(g_mutex);

    // Проверяем и очищаем звуки, которые закончили играть (и не зациклены)
    for (int i = g_playingSounds.size() - 1; i >= 0; --i) {
        PlayingSound* playing = g_playingSounds[i];
        // ma_sound_at_end() проверяет, дошел ли звук до конца
        if (ma_sound_at_end(&playing->sound) && !ma_sound_is_looping(&playing->sound)) {
             LOG_DEBUG("Cleaning up finished sound: %s", playing->name.c_str());
             ma_sound_uninit(&playing->sound);
             delete playing;
             g_playingSounds.erase(g_playingSounds.begin() + i);
        }
    }
    return dmExtension::RESULT_OK;
}

// Вызывается при завершении работы расширения
static dmExtension::Result FinalizeMiniaudio(dmExtension::Params* params) {
    dmLogInfo("Finalizing miniaudio...");
    CleanupAllPlayingSounds(); // Останавливаем и очищаем все играющие звуки

    {
        // Очищаем предзагруженные звуки
        std::lock_guard<std::mutex> lock(g_mutex);
        LOG_DEBUG("Unloading %zu preloaded sounds.", g_preloadedSounds.size());
        for (auto& pair : g_preloadedSounds) {
            ma_sound_uninit(&pair.second);
        }
        g_preloadedSounds.clear();
    }

    ma_engine_uninit(&g_maEngine); // Останавливаем движок miniaudio
    dmLogInfo("miniaudio Finalized");
    return dmExtension::RESULT_OK;
}

// Регистрация расширения в Defold
DM_DECLARE_EXTENSION(Miniaudio, "miniaudio", 0, 0, InitializeMiniaudio, UpdateMiniaudio, 0, FinalizeMiniaudio)