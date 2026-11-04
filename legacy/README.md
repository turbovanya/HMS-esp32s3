# Первая версия прошивки

`sketch_jul20a` стояла на устройстве до нового интерфейса: восемь карточек со значениями,
без графиков и индекса. Исходник восстановлен из кэша сборки Arduino IDE, бинарники из той же сборки.

- `sketch_jul20a/sketch_jul20a.ino`: исходник
- `sketch_jul20a/*.bin`: приложение, загрузчик и таблица разделов
- Плата сборки: `esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=dfu,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=4M,PartitionScheme=default,DebugLevel=none,PSRAM=disabled,LoopCore=1,EventsCore=1,EraseFlash=all,JTAGAdapter=default`

Копия, считанная с самой платы (`device_app.bin`), совпала с `sketch_jul20a.ino.bin` побайтово,
поэтому в репозиторий она не попадает.

## Как вернуть

Загрузчик и таблица разделов у обеих версий одинаковые, достаточно записать приложение:

```bash
"%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\4.5.1\esptool.exe" --chip esp32s3 --port COM3 write_flash 0x10000 legacy\sketch_jul20a\sketch_jul20a.ino.bin
```
