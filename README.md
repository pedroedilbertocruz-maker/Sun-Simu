# Sun Simulation (Dusklight mod)

Sol e lua fisicamente baseados para o [Dusklight](https://twilitrealm.dev/).

- **Posição:** calculada a partir da latitude, do dia do ano e do relógio do jogo, com refração atmosférica.
- **Cor e intensidade:** vêm de uma atmosfera Rayleigh + Mie + ozônio. O sol fica branco ao meio-dia e laranja ou vermelho no horizonte, e o céu tem tom azulado e brilho de crepúsculo.
- **Lua:** tem fases, e à noite passa a ser a luz dominante.
- **Sol do jogo:** o mod move o sol e a lua do próprio jogo (sprite, lens flare e luz base) para as posições simuladas.
- **Serviço para outros mods:** exporta o `SunService` (`com.sunsim.sun`), que outros mods de iluminação podem importar.

## Instalação

Baixe o `.dusk` em **Releases** (ou em **Actions → último build → mod-combined**). Em seguida, copie o arquivo para a pasta de mods:

- Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
- Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
- macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`

## Usando o sol em outro mod

Copie `include/sun_mod/sun_service.h` para o seu mod e faça:

```cpp
#include "sun_mod/sun_service.h"
IMPORT_OPTIONAL_SERVICE(SunService, svc_sun);

SunState sun = SUN_STATE_INIT;
if (svc_sun && svc_sun->get_state(mod_ctx, &sun) == MOD_OK) {
    // sun.light_dir, sun.light_color, sun.light_intensity,
    // sun.sky_color, sun.shadow_strength, sun.shadow_tint, ...
}
```

`get_state` retorna `MOD_UNAVAILABLE` quando a simulação está desligada. Nesse caso, use o sol original do jogo.

## Compilar localmente

```
cmake -B build
cmake --build build
```

O `.dusk` gerado fica em `build/mods/`. A versão do Dusklight usada no build é definida por `DUSKLIGHT_VERSION` no `CMakeLists.txt`.
