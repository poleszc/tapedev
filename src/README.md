# TapeDev Driver
## Struktura kodu
- `tapedev_core.h` - definicje struktur `tapedev_device` i `tapedev_section`, niskopoziomowe 
makra do handlowania hardware'u, publiczny interfejs
- `tapedev_uapi.h` - definicje struktur i numerów wywołań IOCTL
- `tapedev_main.c` - inicjalizacja PCI, rejestracja block layer'u, obsługa przerwań, włączanie urządzenia
- `tapedev_blk.c` - block layer, inicjalizacja `gendisk`, konfiguracja kolejek i obsługa rządań
- `tapedev_cmd.c` - zarządzanie urządzeniem (FAST_FWD, EJECT, REWIND, itp.), wypełnianie `pgtable`
- `tapedev_sysfs.c` - implementacja `ioctl` i atrybuty


## Decyzje projektowe i architektura
### Block Layer i Zero-Copy
- Taśma jest urządzeniem sekwencyjnym przetwarzającym jedną instrukcję na raz, `queue_depth` jest ustawione na 1. 
- Flaga `BLK_MQ_F_BLOCKING` została włączona, bo funkcja `tapedev_queue_rq` jest synchroniczna – potrafi czekać na przerwanie oznaczające koniec I/O lub czekać na mutex.
- Sterownik konwertuje wchodzący wektor danych na mapę SGL, którą przesyła do DMA. Fragmentuje listę SGL i zasila 4 KB tablicę stron.

### Wiele taśm
Główna pętla wewnątrz `tapedev_queue_rq` oblicza na bieżąco, na jakiej fizycznej taśmie znajduje się aktualnie przetwarzany blok. Jeśli zapytanie wykracza poza granicę bieżącej kasety, funkcja ustala, ile bloków maksymalnie można zrzucić na pierwszą taśmę, wysyła je w DMA, wyciąga kartę, wciąga kolejną i dogrywa resztę z odpowiednim przesunięciem we wskaźnikach z wektora SGL.

### Synchronizacja i blokady
- `io_mutex` (na sekcje) - zapobiega wyścigom w warstwie blokowej. Obejmuje całą sekcję `tapedev_queue_rq`. Gwarantuje m.in. że użytkownik nie wywoła polecenia "EJECT" (przez ioctl), w trakcie gdy na taśmie odbywa się zapis/odczyt.
- `cmd_mutex` (na sekcje) - blokada niższej warstwy. Obejmuje jedną komendę sprzętową przesyłaną na kartę PCI (np. zapis komendy i czekanie na jej zakończenie). Gwarantuje, że każda sekcja przyjmuje tylko jedno zadanie na raz.
- `dev->slock` - zabezpieczenie `cmd_done` i `cmd_status` między handlerem przerwań i głównym kodem.
- Kolejki oczekujących - czekając na "INIT DONE" bądź na koniec zapisu komendy, proces jest bezpiecznie usypiany przez `wait_event_timeout` i wybudzany sygnałem z przerwania.

### Dodatkowe rzeczy
- Flaga `needs_recovery` ustawiana, po zfailowanym requeście. Stan urządzenia jest wtedy undefined i nie wiemy
jaka karta i na jakiej pozycji siedzi karta w urządzeniu. Ta flaga powoduje za każdym razem pełny przepięcie w .
handlowaniu kolejnego requesta (EJECT, TAKE_TAPE, REWIND)

### Źródła
- https://linux-kernel-labs.github.io/refs/heads/master/labs/block_device_drivers.html
- adlerdev.c - z labów
- https://achilles.tu.kielce.pl/portal/Members/eb68b762b9f54e23b7ad8c0787719cef/operating-systems-2/instructions/os2_laboratory_9.pdf
- https://elixir.bootlin.com/linux/v6.18.5/source/drivers/block/ps3disk.c