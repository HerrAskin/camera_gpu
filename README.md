# Camera Demo

[![CPU CI](https://github.com/HerrAskin/camera_gpu/actions/workflows/ci.yml/badge.svg)](https://github.com/HerrAskin/camera_gpu/actions/workflows/ci.yml)

Многопоточный C++17 видеоконвейер для Linux: захват RTSP по TCP → GPU-декодирование
через NVIDIA NVDEC → Processor → GPU-кодирование через NVENC → H.264 MP4.
Чтение и запись работают в отдельных потоках с ограниченными очередями.
Поддерживаются таймауты сетевого чтения, остановка по Ctrl+C и переподключение
с отдельным MP4 для каждой сессии. При недоступности GPU используется CPU.
В аппаратном режиме изображение остаётся в GPU.

Тот же конвейер читает локальные видеофайлы. Processor пока пропускает кадры
без изменений; mock и CSV используются для тестирования и диагностики.

### Возможности и границы

- Несколько независимых источников, ограниченные очереди, таймауты и восстановление RTSP-соединений.
- Автоматический CPU fallback и принудительный CPU-режим; NVIDIA GPU нужен только для аппаратного пути.
- Проверки жизненного цикла, очередей и отмены I/O, интеграционные тесты CPU/GPU и сетевых отказов — [матрица тестов](#матрица-тестов).
- Измерения полного 4K-конвейера с проверкой числа кадров — [сравнение CPU/GPU](#сравнение-cpu-и-gpu-на-4k-видео); это не оценка равенства качества кодирования.
- Processor пока не выполняет полезную обработку; индекс видеоархива, поиск, HTTP API и UI не реализованы.

Для каждого источника Session запускает отдельный Pipeline:

```text
Файл / RTSP по TCP
    → VideoCapture [поток чтения]
    → ограниченная очередь кадров
    → Processor [поток Pipeline]
    → ограниченная FIFO-очередь записи
    → VideoWriter [поток записи]
    → H.264 MP4
```

По умолчанию очередь чтения вытесняет старые кадры, а переполнение очереди
записи приводит к ошибке без вытеснения принятых кадров. `--keep-all-frames`
включает ожидание места при чтении, `--write-queue-depth 0` — синхронную запись
в потоке Pipeline. Подробнее — [очереди и завершение](#очереди-и-завершение).

Проект развивается для углубления практического опыта работы с FFmpeg,
аппаратным видео и многопоточными системами. Границы применения определяются
описанными сценариями и проверками; готовность к непрерывной промышленной
эксплуатации не заявляется.

## Сборка и запуск

Требуются CMake 3.20+, компилятор C++17, OpenCV (модуль core), pkg-config
и development-пакеты FFmpeg: libavformat, libavcodec, libavutil, libswscale.
Для аппаратного режима нужны NVIDIA GPU, драйвер и FFmpeg с CUDA/NVENC.
Программная запись H.264 требует encoder `libx264`. Захват файлов и RTSP
работает через libavformat; декодирование и кодирование выбирают GPU при доступности.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

### CPU CI

[GitHub Actions](.github/workflows/ci.yml) собирает проект в Debug на Ubuntu 24.04
и запускает `pipeline`, `frame_queue`, `writer_queue`, `io_cancel`,
`cpu_pipeline` и `multi_source` без NVIDIA GPU. Проверки выполняются при push,
pull request или ручном запуске workflow. GPU- и RTSP fault-тесты остаются
отдельными проверками на машине с подходящим оборудованием.

Интеграционные CPU-тесты включаются отдельно через `CAMERA_ENABLE_CPU_TESTS=ON`:

```sh
cmake -S . -B build-ci -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DCAMERA_ENABLE_CPU_TESTS=ON -DCAMERA_ENABLE_GPU_TESTS=OFF
cmake --build build-ci --parallel 2
ctest --test-dir build-ci --output-on-failure --no-tests=error -R '^(pipeline|frame_queue|writer_queue|io_cancel|cpu_pipeline|multi_source)$'
```

Дополнительно нужны Python 3.9+ и утилиты `ffmpeg`/`ffprobe` с encoder `libx264`.
Бейдж отражает результат запусков на GitHub после публикации workflow,
а не результат локальной проверки.
При сбое CI сохраняет диагностические логи CMake/CTest и созданные CPU-тестами
fixtures и результаты в артефакте `cpu-test-diagnostics` на семь дней.

## Несколько источников

```sh
./build/camera_demo --source video1.mp4 --source video2.mkv --source rtsp://camera-host/stream
./build/camera_demo --cpu --source video1.mp4 --output tmp/custom.mp4 --source video2.mkv --output-dir tmp/recordings
```

Каждый `--source` создаёт отдельный Pipeline: VideoCapture → Processor → VideoWriter.
`--output` задаёт target ближайшего предыдущего источника. Без него создаётся
`<имя>_out.mp4`; для RTSP — `sourceN_out.mp4`. Каталог по умолчанию —
`tmp/recordings` относительно рабочего каталога. Его использование явно логируется,
как и выбранный путь каждого Writer. `--output-dir` меняет каталог автоматических
имён; явный `--output` используется как задан. Существующие автоматические имена
и совпадения получают суффикс `_2`, `_3` и т.д. Явный target перезаписывается;
совпадение с любым входным файлом или другим target запрещено.

Session запускает конвейеры параллельно. Ошибка одного источника завершает только
его Pipeline; остальные продолжают работу, итоговый код — 1. Ctrl+C останавливает
все конвейеры и завершает запись. Каждый RTSP-конвейер имеет собственный бюджет
переподключений. Без аргументов и с `--help` выводится подсказка, код — 0.

## Время и FPS

После завершения каждый Pipeline выводит количество переданных Writer кадров,
время в секундах, средний FPS и число кадров, вытесненных очередью чтения.
Время измеряется steady_clock от начала worker, включая открытие источника,
настройку decoder/encoder, обработку, drain и закрытие файлов. FPS = кадры / время.
При ошибке Writer счётчик принятых кадров не гарантирует их сохранение в файле.

Session отдельно показывает общее время всех параллельных конвейеров и
суммарный FPS = сумма кадров / общее время. Это пропускная способность программы,
а не исходный FPS видео. Для RTSP в неё входит ожидание данных и переподключений.

Для сравнения без потерь используй `--keep-all-frames --write-queue-depth 0`:
захват ждёт свободного места в ограниченной очереди, запись синхронная.
Обычный режим по-прежнему сохраняет самые новые кадры. Опция сохранения всех
кадров полезна для файлов; на живом RTSP она может увеличивать задержку,
а буферы источника остаются вне контроля программы.

```sh
./build-gpu-tests/camera_demo --keep-all-frames --write-queue-depth 0 --cpu --source input.mp4 --output tmp/cpu.mp4
./build-gpu-tests/camera_demo --keep-all-frames --write-queue-depth 0 --source input.mp4 --output tmp/gpu.mp4
```

При сравнении CPU/GPU проверяй одинаковое число кадров и число пропусков:
очередь захвата вытесняет старые кадры. На коротких роликах измерение включает
значительную долю инициализации CUDA/NVENC. Фактический decoder отображается при
открытии Pipeline, выбранный encoder — в сообщении VideoWriter; отсутствие
`--cpu` допускает автоматический fallback.

### Сравнение CPU и GPU на 4K-видео

На локальной машине проверен минутный фрагмент `/home/ser/Videos/52.mkv`:
H.264, 3840×2160, 25 FPS, 1508 кадров, длительность по ffprobe — 60,025 с.
Выполнено по два прогона каждого режима, в порядке GPU → CPU → CPU → GPU.

Измерялся **полный конвейер**: открытие источника → чтение → декодирование →
Processor-заглушка → кодирование H.264 → запись MP4 → flush и закрытие потоков.
В обоих режимах target bitrate encoder — 2 Мбит/с; используются текущие настройки
libx264 и NVENC. Это сравнение скорости программы, без оценки равенства качества
изображения разных encoder.

| Режим | Прогон 1, с | Прогон 2, с | Среднее время, с | FPS по среднему времени | Кадров в каждом MP4 | Пропуски |
|---|---:|---:|---:|---:|---:|---:|
| CPU decoder + libx264 | 33,922 | 30,434 | 32,178 | 46,86 | 1508 | 0 |
| NVDEC + NVENC | 11,905 | 11,892 | 11,899 | 126,74 | 1508 | 0 |

**GPU ускорил полный конвейер в 2,70 раза** при этих настройках и на этом видео.
Среднее время — среднее двух прогонов; FPS = 1508 / среднее время.
Во всех четырёх выходных файлах ffprobe декодировал и подтвердил 1508 кадров,
3840×2160 и длительность 60,025 с. Логи подтвердили CUDA decoder и NVENC для GPU,
программный decoder и libx264 для CPU.

Команды из корня репозитория после сборки. Исходный файл локальный; при повторении
на другой машине замени его путь. Фрагмент создаётся без перекодирования:

```sh
mkdir -p tmp/gpu_cpu_benchmark
ffmpeg -hide_banner -loglevel error -y -i /home/ser/Videos/52.mkv -t 60 -map 0:v:0 -c:v copy -an tmp/gpu_cpu_benchmark/input_60s.mkv
./build-gpu-tests/camera_demo --keep-all-frames --queue-depth 4 --write-queue-depth 0 --source tmp/gpu_cpu_benchmark/input_60s.mkv --output tmp/gpu_cpu_benchmark/gpu_1.mp4
./build-gpu-tests/camera_demo --cpu --keep-all-frames --queue-depth 4 --write-queue-depth 0 --source tmp/gpu_cpu_benchmark/input_60s.mkv --output tmp/gpu_cpu_benchmark/cpu_1.mp4
./build-gpu-tests/camera_demo --cpu --keep-all-frames --queue-depth 4 --write-queue-depth 0 --source tmp/gpu_cpu_benchmark/input_60s.mkv --output tmp/gpu_cpu_benchmark/cpu_2.mp4
./build-gpu-tests/camera_demo --keep-all-frames --queue-depth 4 --write-queue-depth 0 --source tmp/gpu_cpu_benchmark/input_60s.mkv --output tmp/gpu_cpu_benchmark/gpu_2.mp4
ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=width,height,nb_read_frames:format=duration -of json tmp/gpu_cpu_benchmark/gpu_1.mp4
```

Последнюю проверку выполни для каждого выходного MP4. Не засчитывай GPU-прогон,
если лог сообщает CPU fallback, есть пропуски или число кадров отличается.
Исходные результаты, логи и файлы находятся в `tmp/gpu_cpu_benchmark/`;
итоги сохранены в `results.json`. Длительный прогон может занять несколько минут.

## Запись RTSP

Перед запуском задай адрес доступного источника в своём терминале. Например:

```sh
export RTSP_URL='rtsp://camera-host:554/stream'
mkdir -p tmp
./build/camera_demo --queue-depth 8 --write-queue-depth 8 --reconnect 3 --transcode "$RTSP_URL" tmp/live.mp4
```

`RTSP_URL` задай в своём терминале; не сохраняй пароль камеры в репозитории.
`--reconnect N` ограничивает дополнительные попытки открытия/чтения за весь запуск;
по умолчанию их нет. Паузы — 1, 2, 4, затем максимум 5 секунд. Ctrl+C прерывает
паузу. `VideoCapture` переподключается внутри своего рабочего потока.
По новой генерации кадра `VideoWriter` завершает текущий MP4 и открывает следующий. Файлы: `live.mp4`, `live_session2.mp4`,
`live_session3.mp4`; существующие дополнительные файлы пропускаются. Первый файл,
как и прежде, перезаписывается. Попытка без кадров может не создать файл, поэтому
номера бывают с пропусками. Timeline каждой записи независим; промежуток
разрыва не заполняется. Ошибка Writer останавливает программу без повторной попытки.
Исчерпание попыток возвращает код 1; пользовательская остановка — 0.

При ошибке чтения запись завершается; без `--reconnect` программа возвращает код 1.
RTSP-захват и запись проверены на реальной камере. Обрывы, восстановление и
остановка дополнительно проверяются локальным RTSP-сервером в интеграционных тестах.

## Файлы и диагностика

```sh
./build/camera_demo --inspect path/to/input.mp4
./build/camera_demo --decode-first path/to/input.mp4
./build/camera_demo --decode path/to/input.mp4 tmp/frames.csv
./build/camera_demo --transcode path/to/input.mp4 tmp/output.mp4
./build/camera_demo --cpu --transcode path/to/input.mp4 tmp/output_cpu.mp4
```

`--inspect` показывает параметры открытого источника, `--decode-first` — первый
декодированный кадр. `--cpu` отключает аппаратное ускорение принудительно.
Без него захват автоматически использует CPU, если CUDA или аппаратный decoder
недоступны. Если NVENC не удалось открыть, writer переносит GPU-кадры на CPU
и использует libx264. Ошибки источника или записи не маскируются этим fallback.
Аудио пока пропускается.

Для MP4 нужны timestamp и положительная duration. Повторяющиеся и обратные
PTS сдвигаются на одну микросекунду после предыдущего выходного PTS; исходный
кадр не изменяется. CLI пропускает начальные RTSP-кадры без timestamp.
В аппаратном режиме сохраняются цветовые параметры и SAR входного кадра.

## API и устройство конвейера

- `VideoCapture` владеет источником, decoder, очередью чтения и переподключением.
  `init(source, options)` возвращает bool и сообщает результат; конструктор лёгкий.
  `read()` без ожидания выдаёт самый старый из оставшихся кадров либо `nullopt`.
  `isFinished()` отличает временно пустую очередь от завершения.
- `Processor::process(Frame&)` пока пропускает кадр без изменений.
- `VideoWriter` владеет encoder, выходным контейнером и очередью записи.
  `init(path, options)`, `write(frame)`, `finish()` составляют публичный сценарий.
  `finish()` обязателен для flush и MP4 trailer; деструктор завершает worker,
  но не заменяет явное завершение записи. CSV сохраняет только метаданные.

`demo::Session::init()` назначает выходы и создаёт Pipeline для каждого источника.
`start()` запускает их рабочие потоки и ждёт завершения всех либо Ctrl+C,
затем вызывает `stop()`. Источники открываются внутри своих worker, чтобы
ошибка открытия одного не блокировала остальные. `stop()` освобождает владельцев с join; повторный
вызов безопасен, в том числе после неудачного `init()`. `exitCode()` сохраняет
код результата для main.

Эти классы не зависят от CLI и переиспользуются через `camera_core`.
`VideoCapture` допускает специализацию виртуальных методов чтения и остановки.
Публичные headers захвата и записи — `video_capture.hpp` и `video_writer.hpp`.

`VideoCapture::Options` задаёт `ioTimeout` (10 секунд), `queueDepth` (8),
`additionalHeldFrames` (резерв поверхностей для потребителей), `reconnectAttempts`
(0) и `preferGpu` (true). RTSP использует TCP. Таймаут ограничивает открытие,
определение потоков и сетевое ожидание worker. `requestStop()` отменяет I/O
через атомарный флаг и FFmpeg interrupt callback. Деструктор выполняет join
перед освобождением ресурсов. Внешние вызовы должны завершиться до уничтожения объекта.

`VideoWriter::Options` задаёт `format` (CSV), `queueDepth` (0 — синхронно)
и `preferGpu` (true). Encoder создаётся по первому кадру. `outputPaths()` возвращает
пути созданных выходных файлов, включая сегменты после переподключений.

`Frame` хранит CPU `image` (BGR8 для реального источника) либо GPU `gpuImage`
с разделяемым владением AVFrame. Необязательные `timestamp` и `duration` выражены
в микросекундах. `streamGeneration` отличает подключения с независимыми timeline
и позволяет writer автоматически сменить файл. Копия кадра сохраняет его память
после следующего чтения и уничтожения capture. Ошибка чтения передаётся после
выдачи накопленных кадров; EOF файла завершает decoder через drain.

## Структура и изучение кода

Объявления — `include/camera/`, реализации — `src/camera/`, CLI — `apps/`,
проверки — `tests/`. Временные файлы находятся в `tmp/`, сборка — в `build/`.

1. `apps/main.cpp` регистрирует Ctrl+C, разбирает CLI через `ArgumentsParser`, создаёт `demo::Session` и вызывает `init()` → `start()` → `stop()`.
2. `apps/session.cpp`: управление набором Pipeline; `apps/pipeline.cpp`: одна тройка capture/processor/writer и цикл кадров.
3. `VideoCapture::init()` и `openDecoder()`: источник, выбор GPU/CPU, decoder.
4. `readerLoop()` → `readWithRetry()` → `decodeFrame()` → `feedDecoder()` /
   `makeFrame()`: чтение пакетов, декодирование, помещение кадров в очередь.
5. `VideoWriter::writeVideo()`: сегмент, encoder, подготовка кадра, packets в MP4.
6. `VideoWriter::finishVideo()`: encoder flush, остатки packets, trailer.

## Проверки

Mock позволяет проверить базовую сборку без камеры и GPU:

```sh
mkdir -p tmp
./build/camera_demo tmp/mock_frames.csv
```

Опциональная интеграционная проверка требует GPU, FFmpeg с `libx264` и `h264_nvenc`,
а также Python 3.9+. Она создаёт воспроизводимые MP4 fixtures и проверяет транскодирование,
PTS, длительность, цветовые метаданные и полное декодирование результата. По умолчанию
она выключена и не добавляет зависимостей к CPU-тестам:

```sh
cmake -S . -B build-gpu-tests -DCAMERA_ENABLE_GPU_TESTS=ON
cmake --build build-gpu-tests --parallel 2
ctest --test-dir build-gpu-tests -L gpu --output-on-failure
```

Проверка нескольких источников: `ctest --test-dir build-gpu-tests -R multi_source --output-on-failure`.
Она проверяет имена, явные target, совпадения, подсказку и независимость отказов.

CPU-проверка без GPU: `ctest --test-dir build-gpu-tests -R cpu_pipeline --output-on-failure`.
Она проверяет принудительный CPU-режим, кадры, PTS и декодируемость MP4.

Fixture и выходные файлы остаются в `tmp/gpu_pipeline_tests/`.

В той же GPU-сборке на Linux доступна проверка сетевых отказов:

```sh
ctest --test-dir build-gpu-tests -R '^rtsp_faults$' --output-on-failure
```

Тест создаёт локальный RTSP/TCP-источник с H.264 и проверяет отсутствие новых данных,
разрыв TCP, переподключение, исчерпание попыток и Ctrl+C во время ожидания.
Проверяется и восстановление после неудачного первого открытия источника.
Нужны доступ к GPU и loopback-сокетам;
ферма, SSH-туннель и дополнительные Python-пакеты не требуются. Проверяются код
завершения, ограниченное время остановки и полное декодирование полученных MP4.
Логи, видео и измерения находятся в `tmp/rtsp_fault_tests/`; тест ограничен 120 секундами.
Неожиданный EOF RTSP считается ошибкой чтения, EOF файла — штатным завершением.

### Матрица тестов

| CTest | Что проверяет | Нужен GPU |
|---|---|---|
| `pipeline` | Mock-конвейер, жизненный цикл, владение кадрами и запись CSV | Нет |
| `frame_queue` | FIFO, переполнение, таймауты, пробуждение и конкурентный доступ | Нет |
| `writer_queue` | Асинхронная запись, переполнение, завершение worker и CPU MP4 при наличии libx264 | Нет |
| `io_cancel` | Таймауты и отмена сетевого I/O на Linux, loopback-сокеты | Нет |
| `cpu_pipeline` | Принудительный CPU-режим, кадры, PTS и декодируемость MP4 | Нет |
| `multi_source` | Несколько источников, выходные пути и независимость отказов | Нет |
| `gpu_pipeline` | Аппаратный видеопуть, timestamps и свойства выходного видео | Да |
| `rtsp_faults` | Сетевые отказы, переподключение, остановка и декодируемость сегментов | Да |

Первые четыре теста включены в обычную сборку (`io_cancel` — на Unix).
`cpu_pipeline` и `multi_source` включаются через `CAMERA_ENABLE_CPU_TESTS=ON`.
Для совместимости `CAMERA_ENABLE_GPU_TESTS=ON` по-прежнему включает все
интеграционные тесты, включая CPU. Обе опции по умолчанию выключены.
Команды и зависимости приведены выше; эта таблица описывает покрытие,
а не результат нового прогона.

## Очереди и завершение

`FrameQueue` — потокобезопасная очередь с глубиной, заданной при создании:

```cpp
camera::FrameQueue frames(8);
frames.push(std::move(frame));
auto nextFrame = frames.pop(std::chrono::milliseconds(100));
```

Нужен header `camera/frame_queue.hpp`. При переполнении удаляется старейший кадр;
`droppedCount()` считает вытеснения. `pop()` без аргумента не ждёт, с таймаутом
ждёт поступления кадра или закрытия очереди. `nullopt` означает отсутствие кадра:
`isClosed()` отличает закрытую очередь от истечения ожидания. `close()` запрещает
новые кадры, будит ожидающих и оставляет накопленные кадры для чтения. Глубина
должна быть положительной. Все потоки, использующие очередь, нужно завершить до
её уничтожения. Проверка: `ctest --test-dir build -R frame_queue --output-on-failure`.

Очередь чтения подключена внутри VideoCapture, Processor вызывается приложением. Глубину
можно задать перед режимом: `./build/camera_demo --queue-depth 4 --transcode input.mp4 output.mp4`.
При переполнении удаляются старейшие кадры, в том числе при чтении файла быстрее
обработки; это режим актуальных кадров, а не гарантия сохранения каждого кадра.
Ctrl+C завершает текущую итерацию, запрашивает остановку чтения и дописывает MP4.

VideoWriter также поддерживает отдельный поток и ограниченную FIFO-очередь. В CLI она
включена с глубиной 8; `--write-queue-depth N` меняет глубину, `0` возвращает
синхронную запись. При переиспользовании библиотеки третий аргумент `VideoWriter::init`
по умолчанию равен 0. `write()` принимает кадр в очередь без копирования пикселей;
его содержимое после передачи менять нельзя. `finish()` закрывает приём, ждёт
записи принятых кадров и join, затем дописывает trailer. Деструктор выполняет join,
но не заменяет явный finish для MP4.
Первый MP4-кадр каждого сегмента ждёт настройки encoder в worker; это не даёт стартовому
буферу RTSP заполнить очередь во время открытия NVENC. Следующие кадры передаются
без ожидания кодирования.

Для записи используется `FrameQueue::tryPush()`: полная очередь отклоняет кадр
с явной ошибкой, сохраняя уже принятые. CLI завершает принятую часть записи и
останавливается с кодом 1. Ошибки worker передаются через следующий write или finish.
Для GPU VideoCapture резервирует буферы и под очередь Writer. Проверка:
`ctest --test-dir build -R writer_queue --output-on-failure`.

Глубины GPU-очередей нельзя увеличивать произвольно: на проверенном GPU NVDEC
отклонил запрос 36 поверхностей. GPU fixture из 10 кадров проверен с очередями
10/10, обычный CLI использует 8/8. Часть поверхностей нужна самому decoder;
резерв зависит и от исходного codec. См. [реализацию NVDEC в FFmpeg](https://ffmpeg.org/doxygen/6.0/nvdec_8c_source.html).
