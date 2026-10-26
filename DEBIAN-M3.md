# Waybar GTK4 en Debian 13 (MacBook Pro M3 Max, Hyprland)

Rama `gtk4-debian13-m3`: el port a GTK4 de Viktar Lukashonak (Alexays/Waybar#5366, guardado aquí como
`gtk4-upstream-pr5366`) más los cambios necesarios para compilarlo y usarlo en Debian 13:

1. `meson.build`: gtkmm-4.0 ≥ 4.18 y gtk4-layer-shell ≥ 1.0.4 (lo que trae Debian 13), y `giomm-2.68` en vez de `giomm-2.4`.
2. `Glib::Error` en vez de `Glib::Exception` (no existe en glibmm-2.68).
3. `hyprland/workspaces`: guarda del controlador de scroll (segfault al arrancar con `"disable-scroll": true`).

## Compilar e instalar

```sh
sudo apt install --no-install-recommends ninja-build libgtkmm-4.0-dev libgtk4-layer-shell-dev libfmt-dev libspdlog-dev \
  libjsoncpp-dev libsigc++-3.0-dev libnl-3-dev libnl-genl-3-dev libpulse-dev libupower-glib-dev libplayerctl-dev \
  libpipewire-0.3-dev libwireplumber-0.5-dev scdoc libepoxy-dev libhowardhinnant-date-dev libmpdclient-dev \
  libxkbcommon-dev/trixie-backports libxkbregistry-dev/trixie-backports libatk-bridge2.0-dev/trixie-backports libatk1.0-dev/trixie-backports
python3 -m venv ~/venv-meson && ~/venv-meson/bin/pip install 'meson>=1.11.1'   # Debian 13 trae meson 1.7
~/venv-meson/bin/meson setup build --wrap-mode=nodownload -Dcava=disabled -Dtests=disabled -Dgps=disabled -Djack=disabled
ninja -C build -j6
sudo ninja -C build install        # /usr/local/bin/waybar, por delante del waybar de Debian en el PATH
```

Las cuatro `-dev` de backports solo hacen falta si sus librerías ya vienen de backports (aquí sí, por Hyprland).

## Diferencia a tener en cuenta
Desde waybar 0.13 cada barra nombra su capa con su `name` (ya no `waybar`): los scripts que buscan la capa por
namespace tienen que buscar por PID o por el nombre de la barra.

## CSS: no usar `*` con propiedades de fuente
En GTK4 el nombre CSS del módulo (`#custom-foo`) va en la caja del módulo y la etiqueta de texto es hija. Un selector
universal `* { font-family: …; font-size: … }` se aplica directamente a la etiqueta y gana a lo que se ponga en
`#custom-foo`, y la caída automática de glifos elige la variante «Mono» de las Nerd Fonts (iconos estrechos). Poner la
fuente en `window#waybar { … }` para que se herede, y listar las fuentes de iconos explícitamente:
`font-family: "Noto Sans", "Hack Nerd Font", "FontAwesome", sans-serif;`

## Menús de los módulos (`menu`, `menu-file`, `menu-actions`)
El port no los traía (GTK4 no tiene GtkMenu). En esta rama `AModule` lee el `menu-file` con el formato GTK3 documentado
(GtkMenu con GtkMenuItem/GtkSeparatorMenuItem), monta un `Gio::Menu` por secciones y lo muestra en un `Gtk::PopoverMenu`
anclado al módulo; cada elemento ejecuta su entrada de `menu-actions`. CSS: `popover.menu > contents`,
`popover.menu modelbutton`, `popover.menu modelbutton:hover`, `popover.menu separator` (antes `menu`, `menuitem`).
