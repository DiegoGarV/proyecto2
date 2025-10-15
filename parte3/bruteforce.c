// bruteforce.c — DES/ECB (TI-RPC keymap) con MPI, sin padding (longitudes múltiplo de 8)
//
// Compilar (Ubuntu/WSL):
//   sudo apt-get install -y libopenmpi-dev openmpi-bin libssl-dev
//   mpicc bruteforce.c -o bruteforce -lcrypto
//
// Ejemplos:
//   # 1) Encriptar BEN10 interno -> ct.hex (texto) y convertir a bin
//   mpirun -np 1 ./bruteforce -mode encrypt -k 424242 > ct.hex
//   xxd -r -p ct.hex > ct.bin
//
//   # 2) Desencriptar archivo binario con clave conocida
//   mpirun -np 1 ./bruteforce -mode decrypt -k 424242 -in ct.bin > rec.bin
//
//   # 3) Brute-force sobre tu ct.bin usando crib
//   mpirun -np 2 ./bruteforce -mode brute -in ct.bin -crib " secretos " -bits 24
//
//   # 4) Brute sobre bytes del compa con crib
//   BYTES="{241, 49, 35, 6, 25, 159, 151, 68, 69, 50, 237, 92, 45, 89, 154, 75, 134, 19, 232, 4, 94, 205, 139, 65, 80, 251, 236, 242, 243, 42, 181, 34, 218, 125, 137, 244, 95, 150, 190, 120}"
//   mpirun -np 2 ./bruteforce -mode brute -bytes "$BYTES" -crib " Andres " -bits 24

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

// ---------- Shim de compatibilidad TI-RPC usando OpenSSL DES ----------
#include <openssl/des.h>

#ifndef DES_ENCRYPT
#define DES_ENCRYPT 1
#endif
#ifndef DES_DECRYPT
#define DES_DECRYPT 0
#endif
#ifndef DESERR_NONE
#define DESERR_NONE 0
#endif

// Emula des_setparity() de TI-RPC con paridad impar de OpenSSL
static void des_setparity(char *k8) {
    DES_cblock *kb = (DES_cblock*)k8;
    DES_set_odd_parity(kb);
}

// Emula ecb_crypt() de TI-RPC sobre todo el buffer (len múltiplo de 8)
static int ecb_crypt(char *k8, char *buf, unsigned len, unsigned mode) {
    if (len % 8) return 1; // error: no múltiplo de 8
    DES_key_schedule ks;
    DES_cblock *kb = (DES_cblock*)k8;
    DES_set_key_unchecked(kb, &ks);
    for (unsigned i=0; i<len; i+=8) {
        DES_cblock in, out;
        memcpy(&in, buf + i, 8);
        DES_ecb_encrypt(&in, &out, &ks, (mode==DES_ENCRYPT)? DES_ENCRYPT : DES_DECRYPT);
        memcpy(buf + i, &out, 8);
    }
    return DESERR_NONE;
}
// ---------------------------------------------------------------------

#define BLK 8
#define MAXB (1<<20) // 1 MiB

// ====== Plaintext BEN10 interno (múltiplo de 8, sin padding) ======
static const unsigned char BEN10_PLAIN[] =
"Del espacio le llego algo muy especial\n"
"Y lo atrapo y todos sus secretos el sabra\n"
"Con superpoderes el cambio y ahora es\n"
"Ben 10 (B"; // recortado a múltiplo de 8

// ====== Cipher DEMO por si no pasas nada ======
static const unsigned char DEMO_CIPHER[] = {
    0x6C,0xF5,0x41,0x3F,0x7D,0xC8,0x96,0x42,  0x11,0xAA,0xCF,0xAA,0x22,0x1F,0x46,0xD7
};

static void die(const char* msg){
    fprintf(stderr, "%s\n", msg);
    MPI_Abort(MPI_COMM_WORLD, 1);
}

// ================ Mapeo de clave TI-RPC (igual al original) ================
// long k=0; for i=0..7 { key <<= 1; k += (key & (0xFE << i*8)); } des_setparity(&k);
static void tirpc_make_key(uint64_t key_in, unsigned char out8[8]){
    uint64_t key = key_in;
    uint64_t k = 0;
    for (int i=0;i<8;i++){
        key <<= 1;
        k += (key & (0xFEULL << (i*8)));
    }
    des_setparity((char*)&k);   // paridad impar por byte (emulado con OpenSSL)
    memcpy(out8, &k, 8);        // endianness nativa (como lo usa ecb_crypt)
}

static void tirpc_ecb_crypt(uint64_t key56, unsigned char* data, int len, int do_encrypt){
    if (len % BLK) die("Longitud no múltiplo de 8 (sin padding)");
    unsigned char k8[8];
    tirpc_make_key(key56, k8);
    unsigned mode = do_encrypt ? DES_ENCRYPT : DES_DECRYPT;
    int rc = ecb_crypt((char*)k8, (char*)data, (unsigned)len, mode);
    if (rc != DESERR_NONE){
        die("ecb_crypt fallo");
    }
}

// ================= Helpers de I/O =================
static int from_hex(const char* hx, unsigned char* out, int max) {
    int L = (int)strlen(hx);
    if (L%2) return -1;
    int n=0; unsigned v;
    for (int i=0;i<L;i+=2){
        if(n>=max) return -2;
        if (sscanf(&hx[i], "%2x", &v)!=1) return -3;
        out[n++]=(unsigned char)v;
    }
    return n;
}

// parsea: "{241, 49, 0x23, 6,...}" o "241,49,35,6"
static int from_bytes_list(const char* s, unsigned char* out, int max){
    const char* p = s;
    int n=0;
    while (*p){
        while (*p && !(isdigit((unsigned char)*p) || *p=='0')) p++;
        if (!*p) break;
        char* endp;
        long v = strtol(p, &endp, 0); // base 0: dec/hex
        if (endp==p){ p++; continue; }
        if (v<0 || v>255) return -1;
        if (n>=max) return -2;
        out[n++] = (unsigned char)v;
        p = endp;
    }
    return n;
}

static int read_file(const char* path, unsigned char* buf, int max){
    FILE* f=fopen(path,"rb"); if(!f) return -1;
    int n=(int)fread(buf,1,max,f); fclose(f); return n;
}

static void print_hex(const unsigned char* b, int n){
    for (int i=0;i<n;i++) printf("%02x", b[i]);
    printf("\n");
}

// ================= Crib matching =================
static int memmem_naive(const unsigned char* hay, int hlen, const unsigned char* ndl, int nlen){
    if (nlen<=0 || hlen<=0 || nlen>hlen) return 0;
    for (int i=0;i<=hlen-nlen;i++){
        if (memcmp(hay+i, ndl, nlen)==0) return 1;
    }
    return 0;
}

static int contains_crib_any(const unsigned char* p, int n,
                             const char* crib_text,
                             const unsigned char* crib_bytes, int crib_bytes_len){
    if (crib_bytes && crib_bytes_len>0){
        return memmem_naive(p, n, crib_bytes, crib_bytes_len);
    }
    if (crib_text && *crib_text){
        char *tmp = (char*)malloc(n+1);
        if (!tmp) return 0;
        memcpy(tmp, p, n); tmp[n]=0;
        int ok = strstr(tmp, crib_text) != NULL;
        free(tmp);
        return ok;
    }
    return 1;
}

// ================= Wrappers encrypt/decrypt/tryKey =================
static void decrypt_des(uint64_t key, unsigned char *buf, int len){
    tirpc_ecb_crypt(key, buf, len, 0);
}
static void encrypt_des(uint64_t key, unsigned char *buf, int len){
    tirpc_ecb_crypt(key, buf, len, 1);
}
static int tryKey(uint64_t key, const unsigned char *ciph, int len,
                  const char* crib_text,
                  const unsigned char* crib_bytes, int crib_bytes_len){
    unsigned char* tmp = (unsigned char*)malloc(len);
    if (!tmp) return 0;
    memcpy(tmp, ciph, len);
    tirpc_ecb_crypt(key, tmp, len, 0);
    int ok = contains_crib_any(tmp, len, crib_text, crib_bytes, crib_bytes_len);
    free(tmp);
    return ok;
}

// ================= main =================
int main(int argc, char *argv[]){
    MPI_Init(&argc, &argv);
    int N=1, id=0; MPI_Comm comm = MPI_COMM_WORLD;
    MPI_Comm_size(comm, &N);
    MPI_Comm_rank(comm, &id);

    const char* mode=NULL, *crib_text=" the ", *in_path=NULL, *hex_in=NULL, *bytes_in=NULL, *cribhex=NULL;
    uint64_t key=0; int have_key=0; int bits=24; int trim=0;

    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"-mode") && i+1<argc) mode = argv[++i];
        else if (!strcmp(argv[i],"-k") && i+1<argc){ key = strtoull(argv[++i], NULL, 10); have_key=1; }
        else if (!strcmp(argv[i],"-crib") && i+1<argc) crib_text = argv[++i];
        else if (!strcmp(argv[i],"-cribhex") && i+1<argc) cribhex = argv[++i];
        else if (!strcmp(argv[i],"-bits") && i+1<argc) bits = atoi(argv[++i]);
        else if (!strcmp(argv[i],"-in") && i+1<argc) in_path = argv[++i];
        else if (!strcmp(argv[i],"-hex") && i+1<argc) hex_in = argv[++i];
        else if (!strcmp(argv[i],"-bytes") && i+1<argc) bytes_in = argv[++i];
        else if (!strcmp(argv[i],"-trim")) trim = 1;
    }

    if (!mode){
        if (id==0) fprintf(stderr,
            "Uso:\n"
            "  mpirun -np P ./bruteforce -mode encrypt -k <clave> [-in | -hex HEX | -bytes \"{..}\"] [-trim]\n"
            "  mpirun -np P ./bruteforce -mode decrypt -k <clave> [-in | -hex HEX | -bytes \"{..}\"] [-trim]\n"
            "  mpirun -np P ./bruteforce -mode brute   [-in | -hex HEX | -bytes \"{..}\"] [-crib TXT|-cribhex HEX] [-bits 24] [-trim]\n"
            "Notas: DES/ECB (keymap TI-RPC), sin padding. Longitud debe ser múltiplo de 8. Con -trim recorta al múltiplo inferior.\n");
        MPI_Finalize(); return 1;
    }

    // Cargar entrada
    unsigned char *buf=(unsigned char*)malloc(MAXB);
    if(!buf) die("mem");
    int n=0;

    if (hex_in){ n = from_hex(hex_in, buf, MAXB); if(n<0) die("HEX invalido"); }
    else if (bytes_in){ n = from_bytes_list(bytes_in, buf, MAXB); if(n<0) die("Lista de bytes invalida"); }
    else if (in_path){ n = read_file(in_path, buf, MAXB); if(n<0) die("No pude leer -in"); }
    else {
        // Por defecto: encrypt usa BEN10; decrypt/brute usa DEMO_CIPHER
        if (!strcmp(mode,"encrypt")){
            memcpy(buf, BEN10_PLAIN, sizeof(BEN10_PLAIN)-1); // -1 para no incluir '\0'
            n = (int)(sizeof(BEN10_PLAIN)-1);
        } else {
            memcpy(buf, DEMO_CIPHER, sizeof(DEMO_CIPHER));
            n = (int)sizeof(DEMO_CIPHER);
        }
    }

    if (trim) n -= (n % 8);
    if (n<=0 || (n%8)!=0) die("Entrada no es múltiplo de 8 (usa -trim o recorta)");

    // preparar crib por bytes si -cribhex
    unsigned char crib_b[512]; int crib_blen=0;
    if (cribhex && *cribhex){
        crib_blen = from_hex(cribhex, crib_b, (int)sizeof(crib_b));
        if (crib_blen<0) die("cribhex invalido");
    }

    if (!strcmp(mode,"encrypt")){
        if (!have_key) die("Falta -k");
        encrypt_des(key, buf, n);
        if (id==0) print_hex(buf, n);
        free(buf); MPI_Finalize(); return 0;
    }

    if (!strcmp(mode,"decrypt")){
        if (!have_key) die("Falta -k");
        decrypt_des(key, buf, n);
        if (id==0){ fwrite(buf,1,n,stdout); if (buf[n-1]!='\n') printf("\n"); }
        free(buf); MPI_Finalize(); return 0;
    }

    if (!strcmp(mode,"brute")){
        // Broadcast del buffer a todos los ranks
        MPI_Bcast(&n,1,MPI_INT,0,MPI_COMM_WORLD);
        MPI_Bcast(buf,n,MPI_UNSIGNED_CHAR,0,MPI_COMM_WORLD);

        uint64_t maxk = (bits>=56) ? ((1ULL<<56)-1ULL) : ((1ULL<<bits)-1ULL);
        uint64_t found_local=0, found_global=0;
        int f_local=0, f_global=0;

        for (uint64_t k=id; k<=maxk; k+=(uint64_t)N){
            if (tryKey(k, buf, n, (crib_blen>0?NULL:crib_text), (crib_blen>0?crib_b:NULL), crib_blen)){
                found_local=k; f_local=1;
            }
            // parada cooperativa no saturante
            if (f_local || (k % 4096 == (uint64_t)id)){
                MPI_Allreduce(&f_local,&f_global,1,MPI_INT,MPI_MAX,MPI_COMM_WORLD);
                if (f_global){
                    MPI_Allreduce(&found_local,&found_global,1,MPI_UNSIGNED_LONG_LONG,MPI_MAX,MPI_COMM_WORLD);
                    break;
                }
            }
        }

        if (id==0){
            if (f_global){
                unsigned char* plain=(unsigned char*)malloc(n);
                if(!plain) die("mem");
                memcpy(plain, buf, n);
                decrypt_des(found_global, plain, n);
                printf("FOUND_KEY: %llu\n", (unsigned long long)found_global);
                fwrite(plain,1,n,stdout); printf("\n");
                free(plain);
            } else {
                printf("No se encontro clave en 2^%d\n", bits);
            }
        }
        free(buf); MPI_Finalize(); return 0;
    }

    if (id==0) fprintf(stderr,"Modo desconocido\n");
    free(buf); MPI_Finalize(); return 1;
}
