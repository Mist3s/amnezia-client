# HomeVPN

Форк [AmneziaVPN](https://github.com/amnezia-vpn/amnezia-client) для своих устройств.
Отличий от оригинала два:

1. **Конфиг с бэкенда.** Приложение само забирает актуальный конфиг AmneziaWG со своего бэкенда:
   по кнопке, по расписанию и перед подключением. Сменили сервер на бэкенде — устройства переехали сами.
2. **Обновления с бэкенда.** Новая версия приложения скачивается со своего бэкенда и ставится
   только после проверки sha256 из манифеста.

Всё остальное работает как в оригинале. Пока адрес бэкенда не задан, приложение ведёт себя как AmneziaVPN.

Бэкенд — отдельный репозиторий `homevpn-backend`. Протокол: HTTPS + JSON, у каждого устройства свой ключ.

## Что изменено

Новый код лежит в отдельных файлах, правки в файлах оригинала минимальные.

| Что | Где |
| --- | --- |
| Правила: порядок адресов бэкенда, «применять ли конфиг», расписание | `client/core/utils/managedConfig/managedConfigPolicy.*` |
| Правила обновления: разбор манифеста, сравнение версий, sha256 | `client/core/utils/managedConfig/managedUpdatePolicy.*` |
| Хранение настроек (зашифровано вместе с серверами) | `client/core/repositories/managedConfigRepository.*` |
| Получение и применение конфига | `client/core/controllers/managedConfigController.*` |
| Обновление приложения | `client/core/controllers/backendUpdateController.*` |
| Страница настроек | `client/ui/qml/Pages2/PageSettingsManagedConfig.qml`, `client/ui/controllers/managedConfigUiController.*` |
| Тесты | `client/tests/testManagedConfigPolicy.cpp`, `client/tests/testManagedUpdatePolicy.cpp` |
| Сборка | `homevpn/android-apk.sh`, `homevpn/conan-pin-zlib.profile`, `.github/workflows/homevpn-windows.yml` |

Проверка сертификата — штатный TLS Qt, sha256 — `QCryptographicHash`, установка APK — системный установщик Android.
Своей криптографии в форке нет.

## Как настроить устройство

1. Установить HomeVPN.
2. Добавить сервер обычным способом — импортировать любой рабочий `.conf` AmneziaWG.
3. Настройки сервера → **Config auto-update**: вписать адрес бэкенда (`https://…`, можно несколько через запятую)
   и ключ устройства, нажать **Save**, затем **Update config**.

Дальше приложение обновляет конфиг само. Бэкенд недоступен — работает последний полученный конфиг,
соединение не рвётся. Если новый конфиг пришёл при включённом VPN — приложение переподключится.

## Как собрать

Общее для всех платформ — как в оригинале, см. раздел «Hacking guide» в [README.md](README.md):
Qt 6.10, CMake, Ninja, Conan 2, для Android — SDK, NDK 27 и Java 17.

### Версия

Версия HomeVPN = версия оригинала, где последнее число умножено на 100 и к нему прибавлен номер моей сборки.
Оригинал `5.1.1.11`, сборка `1` → `5.1.1.1101`. Номер сборки общий для всех платформ, только растёт
и не сбрасывается, даже когда меняется версия оригинала.

### Android

Нужен ключ подписи (PKCS12). Он один на все сборки и лежит **вне репозитория**: потеряли ключ —
обновления поверх старых установок перестанут ставиться.

```sh
export HOMEVPN_KEYSTORE=/путь/вне/репозитория/homevpn-release.p12
export HOMEVPN_KEYSTORE_PASS_FILE=/путь/вне/репозитория/keystore.pass
export QT_INSTALL_DIR=/каталог/в/котором/лежит/Qt

homevpn/android-apk.sh 30 arm64-v8a   2   # телефоны, Android 11+
homevpn/android-apk.sh 30 armeabi-v7a 2   # приставки, Android 11+
homevpn/android-apk.sh 28 armeabi-v7a 2   # приставки, Android 9–10
```

Аргументы: минимальный Android SDK (`28` — сборка для Android 9–10, `30` — для Android 11 и новее),
архитектура, номер сборки. Для `28` нужен Qt 6.10.1, для `30` — Qt 6.10.3 (так же собирает оригинал).
Готовый файл: `deploy/build-homevpn/out/HomeVPN_<версия>_<abi>_api<sdk>.apk`.

Идентификатор приложения — `dev.homevpn.app`, поэтому HomeVPN ставится рядом с AmneziaVPN и не мешает ему.

### Linux

```sh
EXTRA_CMAKE_ARGS="-DAMNEZIAVPN_VERSION=5.1.1.1102 -DCONAN_HOST_PROFILE=default;auto-cmake;$PWD/homevpn/conan-pin-zlib.profile" \
    deploy/build.sh --generator Ninja --installer all
```

Установщик: `deploy/build/AmneziaVPN_<версия>_linux_x64.run`. Нужен Qt Installer Framework.

Профиль `homevpn/conan-pin-zlib.profile` закрепляет одну версию zlib для всех зависимостей. Без него
`openvpn` и клиент получают две разные сборки OpenSSL, и упаковка установщика падает с ошибкой
`Multiple conflicting paths found for libcrypto.so.3`. Если в кэше Conan уже лежит `openvpn`, собранный
без профиля, его надо удалить: `conan remove "openvpn/*:*" -c`.

### Windows

Собирается в GitHub Actions: Actions → **HomeVPN Windows** → Run workflow → указать номер сборки.
Установщик `AmneziaVPN_<версия>_windows_x64.exe` появится в артефактах запуска.

На ПК сборка не переименована: она называется AmneziaVPN и ставится **вместо** официального клиента.

### Тесты

```sh
cmake --build <каталог сборки> --target test_managed_config_policy test_managed_update_policy
ctest -R 'ManagedConfigPolicyTest|ManagedUpdatePolicyTest'
```

## Как выпустить версию

1. Взять следующий номер сборки (на единицу больше прошлого).
2. Собрать все нужные варианты с этим номером (см. выше).
3. Положить файлы на бэкенд и зарегистрировать — sha256 бэкенд посчитает сам:

   ```sh
   cp HomeVPN_5.1.1.1102_arm64-v8a_api30.apk data/
   ./hvctl release add /data/HomeVPN_5.1.1.1102_arm64-v8a_api30.apk \
       --version 5.1.1.1102 --platform android --abi arm64-v8a --min-sdk 30

   ./hvctl release add /data/HomeVPN_5.1.1.1102_armeabi-v7a_api28.apk \
       --version 5.1.1.1102 --platform android --abi armeabi-v7a --min-sdk 28 --max-sdk 29
   ```

   Для ПК: `--platform linux` (файл `.run`) или `--platform windows` (файл `.exe`).
4. Устройства увидят версию при следующей проверке (после обновления конфига, не чаще раза в 6 часов)
   или сразу по кнопке проверки обновлений в настройках. На Android приложение скачает APK, сверит sha256
   и откроет системное окно установки. При первой установке Android спросит разрешение
   «устанавливать приложения из этого источника» — его надо дать один раз.

Откатить версию: `./hvctl release remove ИМЯ_ФАЙЛА`. Устройства, которые уже обновились, назад не откатятся.

## Как добавить и отозвать устройство

На бэкенде (подробнее — в README бэкенда):

```sh
./hvctl device add tv-zal --from-conf /data/tv-zal.conf --key-file /data/tv-zal.key   # добавить
./hvctl device revoke tv-zal                                                          # отозвать
./hvctl device list                                                                   # кто когда выходил на связь
```

Ключ из `tv-zal.key` вписывается в приложение (см. «Как настроить устройство»), после этого файл удалить.
Отозванное устройство перестаёт получать конфиги и обновления. Чтобы оно перестало подключаться
по старому конфигу, уберите его `[Peer]` на VPN-сервере.

## Как сменить сервер

```sh
./hvctl server add nl2 --from-conf /data/nl2.conf   # один раз, когда сервер новый
./hvctl server activate nl2                         # переключить все устройства
```

Устройства перейдут на новый сервер при следующем опросе (по умолчанию раз в 6 часов с разбросом),
при следующем подключении или сразу по кнопке **Update config**. Если старый сервер перестал отвечать,
приложение само замечает мёртвый туннель и идёт на бэкенд за новым конфигом.

Новый VPN-сервер должен знать все устройства: `./hvctl server peers --out /data/peers.conf`.

## Как подтянуть изменения оригинала

```sh
git fetch upstream
git rebase upstream/dev
```

Конфликты возможны только в немногих тронутых файлах оригинала (`git diff --stat upstream/dev`).

## Ограничения

- На Android опрос идёт, пока жив процесс окна приложения. Если система его выгрузила, конфиг обновится
  при следующем открытии приложения или подключении из него.
- Подключения мимо приложения (Always-on VPN, плитка быстрых настроек) идут с последним сохранённым конфигом,
  без запроса к бэкенду.
- На ПК строгий kill switch блокирует запрос к бэкенду, пока VPN выключен.
- Если адрес бэкенда не задан, проверка обновлений идёт так же, как в оригинале.
