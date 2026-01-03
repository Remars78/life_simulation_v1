#include "raylib.h"
#include "raymath.h"
#include <vector>
#include <thread>
#include <atomic>
#include <random>
#include <algorithm>

// --- КОНСТАНТЫ МИРА ---
const int WORLD_W = 256; // 256 * 128 = 32,768 клеток (хватит для 10k ботов)
const int WORLD_H = 128;
const int GENOME_SIZE = 64;

// Цвета для быстрого доступа
const Color COLOR_EMPTY = {10, 10, 10, 255};
const Color COLOR_ORGANIC = {40, 30, 10, 255};
const Color COLOR_BOT = {0, 255, 0, 255};

struct Bot {
    bool alive = false;
    unsigned char genome[GENOME_SIZE];
    unsigned char ip = 0;       // Instruction Pointer
    unsigned char dir = 0;      // 0-7 directions
    int energy = 0;
    Color color = COLOR_BOT;
};

// Ячейка мира
struct Cell {
    Bot bot;
    int organic = 0; // Органическое вещество (еда)
};

// Два буфера для симуляции (Current и Next)
std::vector<Cell> gridA(WORLD_W * WORLD_H);
std::vector<Cell> gridB(WORLD_W * WORLD_H);
Cell* currentGrid = gridA.data();
Cell* nextGrid = gridB.data();

// Текстура для рендеринга
Image screenImage;
Texture2D screenTexture;
Camera2D camera = { 0 };

// Статистика
std::atomic<int> aliveCount{0};

// Смещения для 8 направлений
const int DIR_X[] = { 0, 1, 1, 1, 0, -1, -1, -1 };
const int DIR_Y[] = { -1, -1, 0, 1, 1, 1, 0, -1 };

// --- ГЕНЕРАЦИЯ ---
void InitWorld() {
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> byteDist(0, 255);
    
    for (int i = 0; i < WORLD_W * WORLD_H; i++) {
        currentGrid[i].organic = byteDist(rng) % 50; // Немного органики везде
        
        // Спавним ботов (примерно 20% заполнения)
        if (byteDist(rng) > 200) {
            currentGrid[i].bot.alive = true;
            currentGrid[i].bot.energy = 500;
            currentGrid[i].bot.dir = byteDist(rng) % 8;
            for (int g = 0; g < GENOME_SIZE; g++) {
                currentGrid[i].bot.genome[g] = byteDist(rng);
            }
        }
    }
}

// --- ВИРТУАЛЬНАЯ МАШИНА (ЛОГИКА БОТА) ---
void ProcessBot(int idx, Cell* readGrid, Cell* writeGrid) {
    Cell& cell = readGrid[idx];
    Bot& bot = cell.bot;
    
    // Если бот мертв, превращаем в органику
    if (bot.energy <= 0) {
        writeGrid[idx].organic += 50; // Труп разлагается
        writeGrid[idx].bot.alive = false;
        return;
    }

    // Копируем состояние бота в следующий кадр (по умолчанию он остается здесь)
    writeGrid[idx].bot = bot; 
    Bot& nextBot = writeGrid[idx].bot;

    // Лимит выполнения команд за ход (чтобы не завис в бесконечном цикле)
    int commandsExecuted = 0;
    bool turnEnded = false;

    while (commandsExecuted < 10 && !turnEnded) {
        unsigned char cmd = bot.genome[nextBot.ip];
        nextBot.ip = (nextBot.ip + 1) % GENOME_SIZE; // Сдвиг указателя

        // Интерпретация команд (упрощенная)
        // 0-7: Сдвиг IP (безусловный переход)
        if (cmd < 8) {
            nextBot.ip = (nextBot.ip + cmd) % GENOME_SIZE;
        }
        // 10-15: Поворот
        else if (cmd >= 10 && cmd <= 15) {
            nextBot.dir = (nextBot.dir + (cmd - 10)) % 8;
        }
        // 20: Фотосинтез
        else if (cmd == 20) {
            nextBot.energy += 5; // Получаем энергию от солнца
            nextBot.color = {0, 255, 0, 255}; // Зеленеем
            turnEnded = true;
        }
        // 30: Поедание органики под собой
        else if (cmd == 30) {
            if (readGrid[idx].organic > 0) {
                int eat = std::min(readGrid[idx].organic, 20);
                nextBot.energy += eat;
                writeGrid[idx].organic -= eat; // Внимание: тут возможна гонка при многопоточности, но для organic это не критично в визуализации
                nextBot.color = {150, 0, 0, 255}; // Краснеем
            }
            turnEnded = true;
        }
        // 40: Движение / Атака
        else if (cmd == 40) {
            int dx = DIR_X[nextBot.dir];
            int dy = DIR_Y[nextBot.dir];
            
            // Тороидальный мир (зацикленный)
            int nx = (idx % WORLD_W + dx + WORLD_W) % WORLD_W;
            int ny = (idx / WORLD_W + dy + WORLD_H) % WORLD_H;
            int nIdx = ny * WORLD_W + nx;

            if (readGrid[nIdx].bot.alive) {
                // Атака соседа (хищничество)
                nextBot.energy += readGrid[nIdx].bot.energy / 2;
                // Сосед умирает в следующем кадре (мы его "перезаписываем" пустым или убитым, 
                // но в double buffer сложно убить соседа мгновенно. 
                // Упрощение: просто получаем энергию, сосед умрет от голода или мы его съедим как органику позже)
            } else {
                // Движение: перемещаемся в свободную клетку
                // Проверяем, не занята ли клетка в writeGrid (кто-то уже сходил туда?)
                // Для простоты и скорости lock-free: если клетка пуста в readGrid, идем.
                // Коллизии решаются приоритетом (кто первый обработался).
                if (!writeGrid[nIdx].bot.alive) {
                    writeGrid[nIdx].bot = nextBot; // Переносим бота
                    writeGrid[idx].bot.alive = false; // Освобождаем старую
                    nextBot.energy -= 2; // Трата на движение
                }
            }
            turnEnded = true;
        }
        
        commandsExecuted++;
    }

    nextBot.energy -= 1; // Трата на существование
}

// --- ОБНОВЛЕНИЕ МИРА (МНОГОПОТОЧНОЕ) ---
void UpdateWorld() {
    aliveCount = 0;

    // Очищаем nextGrid от ботов (органика остается, копируем её или накапливаем)
    // Для оптимизации просто копируем органику, а ботов обнуляем перед записью?
    // Нет, самый быстрый способ:
    // Мы бежим по ReadGrid. Если там бот -> обрабатываем и пишем в WriteGrid.
    // Если там пусто -> просто копируем уровень органики.
    
    // Предварительная очистка WriteGrid для ботов (важно для логики перемещения)
    // На мобильных лучше делать это чанками, но `memset` очень быстр.
    // Однако, нам нужно сохранить органику.
    
    // Упрощенная многопоточность: делим экран на 4 полосы
    int threadsCount = std::thread::hardware_concurrency();
    if (threadsCount == 0) threadsCount = 4;
    
    std::vector<std::thread> workers;
    int chunkSize = (WORLD_W * WORLD_H) / threadsCount;

    // Сначала очищаем слой ботов в буфере записи (чтобы избежать дублей при движении)
    // В реальном high-performance коде это делают умнее, но здесь memset подойдет
    // memset(nextGrid, 0, sizeof(Cell) * WORLD_W * WORLD_H); <- Нельзя, удалит органику.
    
    // Этап 0: Подготовка сетки (быстрое обнуление флагов живых ботов в целевой сетке)
    // Для демо сделаем однопоточно, это очень быстро.
    for(int i=0; i<WORLD_W*WORLD_H; i++) {
        nextGrid[i].bot.alive = false; 
        nextGrid[i].organic = currentGrid[i].organic; // Копируем органику
    }

    auto workerFunc = [&](int start, int end) {
        int localAlive = 0;
        for (int i = start; i < end; i++) {
            if (currentGrid[i].bot.alive) {
                ProcessBot(i, currentGrid, nextGrid);
                localAlive++;
            } else {
                // Прирост органики случайно
                if (GetRandomValue(0, 1000) > 999) nextGrid[i].organic += 10;
            }
        }
        aliveCount += localAlive;
    };

    for (int i = 0; i < threadsCount; i++) {
        int start = i * chunkSize;
        int end = (i == threadsCount - 1) ? (WORLD_W * WORLD_H) : (start + chunkSize);
        workers.emplace_back(workerFunc, start, end);
    }

    for (auto& t : workers) t.join();

    // Меняем буферы местами
    std::swap(currentGrid, nextGrid);
}

// --- ОТРИСОВКА ---
void DrawWorld() {
    Color* pixels = (Color*)screenImage.data;
    
    // Прямой доступ к пикселям быстрее, чем DrawPixel
    for (int i = 0; i < WORLD_W * WORLD_H; i++) {
        if (currentGrid[i].bot.alive) {
            pixels[i] = currentGrid[i].bot.color;
        } else {
            int org = std::min(currentGrid[i].organic * 2, 255);
            pixels[i] = (Color){(unsigned char)org, (unsigned char)(org/2), 0, 255};
        }
    }
    
    UpdateTexture(screenTexture, pixels);
}

int main() {
    // Инициализация окна
    InitWindow(0, 0, "ALife Sim"); // 0,0 для полного экрана на Android
    SetTargetFPS(60);

    InitWorld();

    // Настройка камеры и текстур
    screenImage = GenImageColor(WORLD_W, WORLD_H, BLACK);
    screenTexture = LoadTextureFromImage(screenImage);
    
    camera.target = { (float)WORLD_W/2.0f, (float)WORLD_H/2.0f };
    camera.offset = { (float)GetScreenWidth()/2.0f, (float)GetScreenHeight()/2.0f };
    camera.rotation = 0.0f;
    camera.zoom = 4.0f;

    while (!WindowShouldClose()) {
        // --- INPUT (Touch / Mouse) ---
        // Zoom
        float wheel = GetMouseWheelMove();
        if (wheel != 0) {
            camera.zoom += wheel;
            if (camera.zoom < 0.1f) camera.zoom = 0.1f;
        }
        
        // Pan (Touch drag)
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 delta = GetMouseDelta();
            delta = Vector2Scale(delta, -1.0f / camera.zoom);
            camera.target = Vector2Add(camera.target, delta);
        }
        
        // Android Touch Zoom (Multitouch simulation logic usually needed, 
        // but basics: drag pan works out of box with mouse simulation)

        // --- UPDATE ---
        UpdateWorld();

        // --- DRAW ---
        BeginDrawing();
            ClearBackground(BLACK);
            
            BeginMode2D(camera);
                DrawTexture(screenTexture, 0, 0, WHITE);
            EndMode2D();

            DrawFPS(10, 10);
            DrawText(TextFormat("Bots: %d", (int)aliveCount), 10, 40, 30, WHITE);
        EndDrawing();
    }

    UnloadTexture(screenTexture);
    UnloadImage(screenImage);
    CloseWindow();

    return 0;
}
