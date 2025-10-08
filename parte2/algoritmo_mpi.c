// Maestro–trabajadores con subprefijos de 2 dígitos (a–z,0–9)
// El maestro reparte tareas, y los trabajadores buscan coincidencias con debug opcional.
// Compilar:  mpicc -O3 -std=c11 -Wall -Wextra -o algoritmo_mpi algoritmo_mpi.c
// Ejecutar:  mpirun -np 4 ./algoritmo_mpi

#include <mpi.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static const char DIGITS[] = "abcdefghijklmnopqrstuvwxyz0123456789";
static const int RADIX = 36;
typedef enum { STRAT_CONTIG = 0, STRAT_SHUFFLE = 1 } Strategy;


// ---------------- Configuración ----------------
typedef struct {
    const char* prefix;
    int len;
    int n_live;
    uint64_t seed;
    int stop_on_first;
    int print_targets;

    // Debug
    int debug;                // 1=on (default), 0=off
    uint64_t progress_step;   // cada cuántos intentos imprimir progreso (default 5e6)

    Strategy strategy;
} Config;

static void die(const char* msg) {
    fprintf(stderr, "Error: %s\n", msg);
    MPI_Abort(MPI_COMM_WORLD, 1);
    exit(1);
}

// ---------------- Utilidades ----------------
static uint64_t powu(uint64_t a, int b) {
    uint64_t r = 1;
    for (int i = 0; i < b; ++i) {
        if (r > UINT64_MAX / a) die("overflow");
        r *= a;
    }
    return r;
}

// RNG para simular “hosts vivos”
static uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint64_t* make_targets(uint64_t total, int n_live, uint64_t seed) {
    if (n_live <= 0) return NULL;
    uint64_t* T = (uint64_t*)malloc(sizeof(uint64_t) * (size_t)n_live);
    if (!T) die("malloc targets");
    uint64_t s = seed ? seed : 1;
    for (int k = 0; k < n_live; ) {
        uint64_t cand = splitmix64(&s) % total;
        int ok = 1;
        for (int j = 0; j < k; ++j) if (T[j] == cand) { ok = 0; break; }
        if (ok) T[k++] = cand;
    }
    return T;
}

static int is_target(uint64_t idx, const uint64_t* T, int n_live) {
    for (int i = 0; i < n_live; ++i) if (T[i] == idx) return 1;
    return 0;
}

// idx → string base36 de longitud fija
static void index_to_base36(uint64_t idx, int len, char* out) {
    for (int i = len - 1; i >= 0; --i) {
        out[i] = DIGITS[idx % RADIX];
        idx /= RADIX;
    }
    out[len] = '\0';
}

static void subprefix_id_to_chars(uint64_t sp_id, char *c0, char *c1) {
    *c1 = DIGITS[sp_id % RADIX];
    sp_id /= RADIX;
    *c0 = DIGITS[sp_id % RADIX];
}

static void print_found(int rank, const char* prefix, int len, uint64_t idx) {
    char suf[64];
    index_to_base36(idx, len, suf);
    printf("[rank %d] FOUND: %s%s (idx=%" PRIu64 ")\n", rank, prefix, suf, idx);
    fflush(stdout);
}

// ---------------- CLI ----------------
static void parse_args(int argc, char** argv, Config* cfg) {
    cfg->prefix = "host-A-";
    cfg->len = 7;
    cfg->n_live = 1;
    cfg->seed = 42;
    cfg->stop_on_first = 1;
    cfg->print_targets = 0;

    cfg->debug = 1;
    cfg->progress_step = 5000000ULL;

    cfg->strategy = STRAT_CONTIG;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--prefix") && i+1 < argc) cfg->prefix = argv[++i];
        else if (!strcmp(argv[i], "--len") && i+1 < argc) cfg->len = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--n_live") && i+1 < argc) cfg->n_live = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i+1 < argc) cfg->seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--stop_on_first") && i+1 < argc) cfg->stop_on_first = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--print_targets") && i+1 < argc) cfg->print_targets = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--debug") && i+1 < argc) cfg->debug = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--progress_step") && i+1 < argc) cfg->progress_step = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--strategy") && i+1 < argc) {
            const char* s = argv[++i];
            if (!strcmp(s, "contig"))      cfg->strategy = STRAT_CONTIG;
            else if (!strcmp(s, "shuffle")) cfg->strategy = STRAT_SHUFFLE;
            else die("valor de --strategy inválido (use 'contig' o 'shuffle')");
        }
    }
    if (cfg->len < 2) die("--len debe ser >= 2");
    if (cfg->n_live < 0) die("--n_live debe ser >= 0");
}

// ---------------- Protocolo MPI ----------------
enum { TAG_REQ = 1, TAG_ASSIGN = 2, TAG_FOUND = 3, TAG_STOP = 4 };

typedef struct {
    uint64_t subprefix_id;
    int      valid;
} AssignMsg;

// ---------------- Maestro ----------------
static void run_master(const Config* cfg, int world, uint64_t total, const uint64_t* targets) {
    (void)total;
    const uint64_t SUBSPACE = powu(RADIX, 2);

    int workers = world - 1;

    // Estado continuo
    uint64_t next_task = 0;

    // Estado barajado
    uint64_t *ids = NULL, pos = 0;
    if (cfg->strategy == STRAT_SHUFFLE) {
        ids = (uint64_t*)malloc((size_t)SUBSPACE * sizeof(uint64_t));
        if (!ids) die("malloc ids barajado");
        for (uint64_t i = 0; i < SUBSPACE; ++i) ids[i] = i;

        // Barajar (Fisher-Yates)
        uint64_t s = cfg->seed ? cfg->seed : 1;
        for (uint64_t i = SUBSPACE - 1; i > 0; --i) {
            uint64_t j = splitmix64(&s) % (i + 1);
            uint64_t t = ids[i]; ids[i] = ids[j]; ids[j] = t;
        }
        if (cfg->debug) {
            printf("[master] Estrategia=SHUFFLE (cola barajada), seed=%" PRIu64 "\n", cfg->seed);
            fflush(stdout);
        }
    } else if (cfg->debug) {
        printf("[master] Estrategia=CONTIG (orden 0..N-1)\n");
        fflush(stdout);
    }

    int stop_broadcasted = 0;

    if (cfg->print_targets && cfg->n_live > 0) {
        printf("[master] Objetivos simulados:\n");
        for (int i = 0; i < cfg->n_live; ++i) {
            char suf[64];
            index_to_base36(targets[i], cfg->len, suf);
            printf("  %s%s\n", cfg->prefix, suf);
        }
        fflush(stdout);
    }

    while (workers > 0) {
        MPI_Status st;

        // ¿Alguien encontró?
        int flag_found = 0;
        MPI_Iprobe(MPI_ANY_SOURCE, TAG_FOUND, MPI_COMM_WORLD, &flag_found, &st);
        if (flag_found) {
            uint64_t idx;
            MPI_Recv(&idx, 1, MPI_UINT64_T, st.MPI_SOURCE, TAG_FOUND, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            print_found(st.MPI_SOURCE, cfg->prefix, cfg->len, idx);

            if (cfg->stop_on_first && !stop_broadcasted) {
                if (cfg->debug) printf("[master] STOP broadcast (primer hallazgo)\n"), fflush(stdout);
                int one = 1;
                for (int p = 1; p < world; ++p)
                    MPI_Send(&one, 1, MPI_INT, p, TAG_STOP, MPI_COMM_WORLD);
                stop_broadcasted = 1;
            }
        }

        // ¿Hay request de trabajo?
        int flag_req = 0;
        MPI_Iprobe(MPI_ANY_SOURCE, TAG_REQ, MPI_COMM_WORLD, &flag_req, &st);
        if (flag_req) {
            uint64_t dummy;
            MPI_Recv(&dummy, 1, MPI_UINT64_T, st.MPI_SOURCE, TAG_REQ, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            AssignMsg msg;
            uint64_t remaining =
                (cfg->strategy == STRAT_SHUFFLE) ? (SUBSPACE - pos) : (SUBSPACE - next_task);
            
            if (stop_broadcasted || remaining == 0) {
                msg.valid = 0;
                msg.subprefix_id = 0;
            } else {
                msg.valid = 1;
                msg.subprefix_id = (cfg->strategy == STRAT_SHUFFLE)
                                     ? ids[pos++]
                                     : next_task++;
            }

            MPI_Send(&msg, sizeof(msg), MPI_BYTE, st.MPI_SOURCE, TAG_ASSIGN, MPI_COMM_WORLD);

            if (cfg->debug) {
                if (msg.valid) {
                    char c0, c1; subprefix_id_to_chars(msg.subprefix_id, &c0, &c1);
                    printf("[master] ASSIGN -> rank %d  subprefijo=%c%c (id=%" PRIu64 ")\n",
                           st.MPI_SOURCE, c0, c1, msg.subprefix_id);
                } else {
                    printf("[master] NO MORE WORK -> rank %d\n", st.MPI_SOURCE);
                }
                fflush(stdout);
            }

            if (!msg.valid) --workers;
        }

        if (!flag_found && !flag_req) {
            double t0 = MPI_Wtime();
            while (MPI_Wtime() - t0 < 0.001) { /* yield ~1 ms */ }
        }

    }

    // Liberar recursos en caso de barajado
    if (ids) free(ids);
}

// ---------------- Trabajador ----------------
static void run_worker(const Config* cfg, int rank, uint64_t total, const uint64_t* targets) {
    (void)total;
    const uint64_t REMSPACE = powu(RADIX, cfg->len - 2);

    while (1) {
        // ¿STOP global?
        int has_stop = 0; MPI_Status stp;
        MPI_Iprobe(0, TAG_STOP, MPI_COMM_WORLD, &has_stop, &stp);
        if (has_stop) { int tmp; MPI_Recv(&tmp, 1, MPI_INT, 0, TAG_STOP, MPI_COMM_WORLD, MPI_STATUS_IGNORE); break; }

        // pedir tarea
        uint64_t req = 1;
        MPI_Send(&req, 1, MPI_UINT64_T, 0, TAG_REQ, MPI_COMM_WORLD);

        // recibir asignación
        MPI_Status st; AssignMsg msg;
        MPI_Recv(&msg, sizeof(msg), MPI_BYTE, 0, TAG_ASSIGN, MPI_COMM_WORLD, &st);
        if (!msg.valid) break;

        char c0, c1; subprefix_id_to_chars(msg.subprefix_id, &c0, &c1);
        uint64_t base_idx = msg.subprefix_id * REMSPACE;

        double t0 = MPI_Wtime();
        uint64_t checked = 0, hits = 0;

        if (cfg->debug) {
            printf("[rank %d] ASSIGN  subprefijo=%c%c (id=%" PRIu64 ")  rango=[%" PRIu64 ", %" PRIu64 ")\n",
                   rank, c0, c1, msg.subprefix_id, base_idx, base_idx + REMSPACE);
            fflush(stdout);
        }

        int found_here = 0;
        for (uint64_t r = 0; r < REMSPACE; ++r) {
            uint64_t idx = base_idx + r;
            if (is_target(idx, targets, cfg->n_live)) {
                ++hits;
                MPI_Send(&idx, 1, MPI_UINT64_T, 0, TAG_FOUND, MPI_COMM_WORLD);
                print_found(rank, cfg->prefix, cfg->len, idx);
                found_here = 1;
                if (cfg->stop_on_first) break;
            }

            ++checked;

            // progreso periódico
            if (cfg->debug && cfg->progress_step && (checked % cfg->progress_step == 0)) {
                double pct = (100.0 * (double)checked) / (double)REMSPACE;
                double dt = MPI_Wtime() - t0;
                printf("[rank %d] PROGRESS  %c%c  %" PRIu64 "/%" PRIu64 " (%.2f%%)  t=%.2fs\n",
                       rank, c0, c1, checked, REMSPACE, pct, dt);
                fflush(stdout);
            }

            // ¿STOP mientras trabajaba?
            int flag_stop = 0;
            MPI_Iprobe(0, TAG_STOP, MPI_COMM_WORLD, &flag_stop, &st);
            if (flag_stop) { int tmp; MPI_Recv(&tmp, 1, MPI_INT, 0, TAG_STOP, MPI_COMM_WORLD, MPI_STATUS_IGNORE); break; }
        }

        double dt = MPI_Wtime() - t0;
        if (cfg->debug) {
            double pct = (100.0 * (double)checked) / (double)REMSPACE;
            printf("[rank %d] DONE     subprefijo=%c%c  checked=%" PRIu64 "/%" PRIu64 " (%.2f%%)  hits=%" PRIu64 "  time=%.2fs\n",
                   rank, c0, c1, checked, REMSPACE, pct, hits, dt);
            fflush(stdout);
        }

        if (cfg->stop_on_first && found_here) break;
    }
}

// ---------------- main ----------------
int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank=0, world=1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world);

    if (world < 2 && rank == 0)
        fprintf(stderr, "Se necesitan al menos 2 procesos (1 maestro + 1 trabajador)\n");

    Config cfg;
    parse_args(argc, argv, &cfg);

    uint64_t total = powu(RADIX, cfg.len);
    uint64_t* targets = make_targets(total, cfg.n_live, cfg.seed);

    if (rank == 0) run_master(&cfg, world, total, targets);
    else           run_worker(&cfg, rank, total, targets);

    free(targets);
    MPI_Finalize();
    return 0;
}