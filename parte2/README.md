# Compilar
mpicc -O3 -std=c11 -Wall -Wextra -o algoritmo_mpi algoritmo_mpi.c

# Ejecutar
mpirun -np 4 ./algoritmo_mpi                          # debug ON por defecto
mpirun -np 4 ./algoritmo_mpi --debug 0                # silenciar mensajes
mpirun -np 1 ./algoritmo_mpi --print_targets 1

## Flags
# --prefix STR
Prefijo del hostname. Se concatena antes del sufijo base-36 que se explora.
Default: host-A-
Ejemplo: --prefix host-B- → genera nombres como host-B-abc1234.

# --len INT
Longitud total del sufijo (caracteres a–z y 0–9) que se brute-forcean.
Default: 7
Ejemplo: --len 4 → subespacio por subprefijo es 36^(4-2)=1296.

# --n_live INT
Cantidad de “hosts vivos” simulados (objetivos a encontrar).
Default: 1
Ejemplo: --n_live 2 simula dos objetivos.

# --seed UINT64
Semilla del RNG para ubicar los índices objetivo de forma reproducible.
Default: 42
Ejemplo: --seed 99.

# --stop_on_first 0|1
Si es 1, cuando algún proceso encuentra un objetivo se detiene todo; si es 0, se sigue explorando hasta agotar tareas.
Default: 1
Ejemplos: --stop_on_first 0 para seguir buscando más coincidencias.

# --print_targets 0|1
Imprime en inicio los objetivos simulados (útil para debug/validar).
Default: 0
Ejemplo: --print_targets 1.