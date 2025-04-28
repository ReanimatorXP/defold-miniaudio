/** @noResolution */

declare namespace miniaudio {
    /**
     * Устанавливает базовый путь для поиска звуковых файлов.
     * @param path Путь к папке со звуками.
     */
    export function set_base_path(path: string): void;

    /**
     * Предзагружает звук (полностью декодирует в память).
     * @param sound_name Имя звука (без расширения).
     * @returns true в случае успеха, false если файл не найден или произошла ошибка.
     */
    export function preload(sound_name: string): boolean;

    /**
     * Выгружает предзагруженный звук из памяти.
     * @param sound_name Имя предзагруженного звука.
     * @returns true если звук был найден и выгружен, false в противном случае.
     */
    export function unload(sound_name: string): boolean;

    /**
     * Воспроизводит звук.
     * @param sound_name Имя звука (без расширения).
     * @param looping Зациклить воспроизведение (по умолчанию false).
     * @param volume Громкость от 0.0 (тишина) до N (1.0 = нормальная, по умолчанию 1.0).
     * @param pitch Высота тона/скорость (1.0 = нормальная, по умолчанию 1.0).
     * @returns true в случае успеха запуска, false если произошла ошибка.
     */
    export function play(sound_name: string, looping?: boolean, volume?: number, pitch?: number): boolean;

    /**
     * Останавливает все играющие экземпляры звука с указанным именем.
     * @param sound_name Имя звука для остановки.
     * @returns Количество остановленных экземпляров звука.
     */
    export function stop(sound_name: string): number;

    /**
     * Останавливает все играющие звуки.
     * @returns Количество остановленных звуков.
     */
    export function stop_all(): number;

    /**
     * Проверяет, играет ли хотя бы один экземпляр звука с указанным именем.
     * @param sound_name Имя звука для проверки.
     * @returns true если хотя бы один экземпляр играет, false в противном случае.
     */
    export function is_playing(sound_name: string): boolean;

    /**
     * Включает или выключает вывод отладочных логов расширения.
     * @param enable true для включения, false для выключения.
     */
    export function set_debug(enable: boolean): void;

    /**
     * Устанавливает общую громкость для всего движка.
     * @param volume Громкость от 0.0 (тишина) до N (1.0 = нормальная).
     */
    export function set_master_volume(volume: number): void;
}
