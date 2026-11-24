# Офлайн-карты Nokia 700

© Участники OpenStreetMap. Данные под лицензией [Open Database License 1.0](https://opendatacommons.org/licenses/odbl/1-0/). [OpenStreetMap и авторские права](https://www.openstreetmap.org/copyright)

Фоновый слой `omsk.bin` содержит упрощённую геометрию дорог, железных дорог, водотоков и населённых пунктов. Границы покрытия от 72,70° до 74,05° в. д. и от 54,68° до 55,32° с. ш. Снимок исходных данных от 21 ноября 2026 года, 00:13:38 UTC. [Выгрузка Омской области OpenStreetMap France](https://download.openstreetmap.fr/extracts/russia/siberian_federal_district/omsk_oblast-latest.osm.pbf) · [Состояние выгрузки](https://download.openstreetmap.fr/extracts/russia/siberian_federal_district/omsk_oblast.state.txt)

В меню «Настройки → Дополнительно → Карты» доступны четыре начальные области. Подложки входят в приложение, открываются без сети и переключаются при перемещении и изменении масштаба. В Омске можно приблизиться до отметки 30 м на линейке и рассмотреть местные улицы и здания. Географические координаты узлов используются, если рация их передала.

| Область | Слой | Наполнение | Размер |
| --- | --- | --- | ---: |
| Омск | `omsk.bin` | Дорожная и железнодорожная сеть, водотоки и 84 населённых пункта | 150 089 Б |
| Подробный Омск | `omsk-detail.bin` | 46 005 контуров зданий, 6 067 линий улиц и 12 061 линия проездов, велодорожек и пешеходных путей | 866 518 Б |
| Омская область | `omsk-oblast.bin` | Основные и региональные дороги, железные дороги, реки, граница области и 609 населённых пунктов | 192 955 Б |
| Сибирь | `siberia.bin` | Обзор Азиатской России и большей части Европейской России, соседние территории, крупные дороги, реки, озёра и 153 населённых пункта | 121 863 Б |
| Мир | `world.bin` | Обзор берегов, границ, крупных рек и озёр, 243 населённых пункта | 35 883 Б |

Данные по Сибири и миру взяты из Natural Earth в масштабах 1:10 000 000, 1:50 000 000 и 1:110 000 000. Natural Earth предоставляет эти данные в общественное достояние. Обзор Сибири охватывает интервал 18–180° в. д. и 40–83° с. ш. Часть России за 180-м меридианом доступна на мировой подложке. [Условия использования Natural Earth](https://www.naturalearthdata.com/about/terms-of-use/) · [Скачивание слоёв](https://www.naturalearthdata.com/downloads/)

## Пересборка подложки Омска

Из исходного `.osm.pbf` извлеките геометрию через GDAL. Погрешность упрощения линий составляет до 0,00007° перед округлением координат до 0,00001°.

```sh
ogr2ogr -f GeoJSON -overwrite roads-water.geojson omsk_oblast.osm.pbf lines \
  -spat 72.68 54.65 74.07 55.35 -clipsrc 72.70 54.68 74.05 55.32 \
  -select name,highway,waterway,railway -simplify 0.00007 \
  -where "highway IN ('motorway','motorway_link','trunk','trunk_link','primary','primary_link','secondary','secondary_link','tertiary','tertiary_link','unclassified') OR waterway IN ('river','canal','stream') OR railway = 'rail'"
ogr2ogr -f GeoJSON -overwrite places.geojson omsk_oblast.osm.pbf points \
  -spat 72.70 54.68 74.05 55.32 -select name,place \
  -where "place IN ('city','town','village') AND name IS NOT NULL"
python3 pack.py
```

При сборке SIS файл `maps.qrc` включает слой в приложение как ресурс `:/maps/omsk.bin`. Слой содержит 5 424 линии, 27 414 записей координат и 84 населённых пункта. Его SHA-256 `023c207348ecad766381153585d7f0037ce37f7f9b1a31071eeb854c287276c3`.

## Подробные улицы и здания Омска

Дополнительный слой охватывает город и ближайшие окрестности между 73,12–73,66° в. д. и 54,83–55,18° с. ш. Он добавляет реальные векторные контуры из того же снимка OpenStreetMap. Самые маленькие здания, большинство дворовых подъездов и парковочных проездов исключены ради размера SIS. Включены здания площадью от 140 м², местные улицы, часть велодорожек, пешеходных путей и служебных проездов. Подписи улиц и номера домов в этот слой не входят.

Данные квантованы на сетке Web Mercator с шагом около 0,7 м на широте Омска. Файл состоит из 563 независимо сжатых секторов. Приложение загружает секторы в пределах окна карты. Наибольший распакованный сектор занимает 11 000 Б до создания структур рисования.

Для пересборки используйте Python 3, GDAL и тот же снимок OSM Омской области.

```sh
python3 resources/maps/pack_detail.py \
  --work-dir /path/to/map-work \
  --osm-pbf /path/to/omsk_oblast.osm.pbf
```

SHA-256 `omsk-detail.bin` — `b25a7577cac819c6bff367db8ae8b8845164aeb23c8b18765e86ac6e680152da`.


## Пересборка областной и обзорных подложек

Запустите Python 3 и GDAL из корня проекта. Ключ `--download-natural-earth` скачивает исходные архивы в рабочий каталог. Если архивы уже есть, запуск работает без сети. Для Омской области используйте тот же снимок OSM, что указан выше.

```sh
python3 resources/maps/pack_regions.py \
  --work-dir /path/to/map-work \
  --osm-pbf /path/to/omsk_oblast.osm.pbf \
  --download-natural-earth
```

Скрипт выбирает и упрощает линии по масштабу, округляет координаты новых слоёв до 0,001° и записывает ресурсы в `resources/maps`. Исходные архивы Natural Earth загружаются с официального сервера проекта. [Natural Earth downloads](https://naturalearth.s3.amazonaws.com/)

- SHA-256 `omsk-oblast.bin` — `a833b56195c57dbefe5be62e229055264a74d6df22584acddde693c47b7999fe`
- SHA-256 `siberia.bin` — `9a6ebc845d5b1b1e58ee4cb08fa6eec8cfe863e9dd183f003ac5eb09c409aa58`
- SHA-256 `world.bin` — `595166344ca660031f0f94caab42173978b6fa0e46d14b5842fd6018ac54faf5`
