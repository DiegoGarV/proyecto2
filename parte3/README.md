# Bruteforce DES - README

## REQUISITOS (UBUNTU / WSL)

Dependencias — copiar y pegar:

```bash
sudo apt-get update
sudo apt-get install -y libopenmpi-dev openmpi-bin libssl-dev
```

> NOTE: OpenSSL puede mostrar warnings por APIs DES “deprecated”. Es normal para este demo — puedes ignorarlos.

---

## COMPILAR

```bash
mpicc bruteforce.c -o bruteforce -lcrypto
```

---

## MODOS DE USO — ENTRADA DE DATOS

Formas de pasar el mensaje / ciphertext:

- `-in archivo` → lee binario (.bin)  
- `-hex "AABBCC..."` → string hex (sin espacios / sin `0x`)  
- `-bytes "{10, 255, 0x1a, ...}"` → lista decimal/hex

Opciones adicionales:

- `-trim` → recorta la entrada al múltiplo de 8 inferior (si no lo es)

---

## ENCRIPTAR

- Con texto interno (ej. *Ben 10*) y clave `424242`:

```bash
mpirun -np 1 ./bruteforce -mode encrypt -k 424242 > ct.hex
xxd -r -p ct.hex > ct.bin      # (opcional) pasar a binario
```

- Encriptar `msg.txt`, recortando si no es múltiplo de 8:

```bash
mpirun -np 1 ./bruteforce -mode encrypt -k 123456 -in msg.txt -trim > ct.hex
xxd -r -p ct.hex > ct.bin
```

---

## DESENCRIPTAR (CLAVE CONOCIDA)

- Desde binario:

```bash
mpirun -np 1 ./bruteforce -mode decrypt -k 424242 -in ct.bin > rec.bin
```

- Desde hex:

```bash
mpirun -np 1 ./bruteforce -mode decrypt -k 424242 -hex "$(cat ct.hex)" > rec.bin
```

---

## BRUTE FORCE (CLAVE DESCONOCIDA)

Requiere un *crib* (pista) presente en el texto en claro.

- Crib textual:

```bash
mpirun -np 2 ./bruteforce -mode brute -in ct.bin -crib " secretos " -bits 24
```

- Crib en hex (ej. `"Andres"` → `20416e6472657320`):

```bash
mpirun -np 2 ./bruteforce -mode brute -in ct.bin -cribhex "20416e6472657320" -bits 28
```

- Si aparece error de “not enough slots” en WSL, usar `--oversubscribe` o bajar `-np`:

```bash
mpirun --oversubscribe -np 4 ./bruteforce ...
```

---

## FORMATOS ÚTILES

### Convertir hex ↔ bin

- Hex → bin:

```bash
xxd -r -p ct.hex > ct.bin
```

- Bin → hex (sin salto de línea final):

```bash
xxd -p ct.bin | tr -d '\n' ; echo
```

### Sacar lista de bytes desde un `.bin` (para `-bytes`)

```bash
BYTES="{$(hexdump -v -e '1/1 "%u, "' ct.bin | sed 's/, $//')}"
echo "$BYTES"
```

Ejemplo con `-bytes`:

```bash
mpirun -np 2 ./bruteforce -mode brute -bytes "$BYTES" -crib " secretos " -bits 24
```

---

## EJEMPLOS RÁPIDOS

### (A) Encriptar (Ben 10 interno) → Brute force

```bash
# Encriptar
mpirun -np 1 ./bruteforce -mode encrypt -k 424242 > ct.hex
xxd -r -p ct.hex > ct.bin

# Brute con crib textual
mpirun -np 2 ./bruteforce -mode brute -in ct.bin -crib " secretos " -bits 24
```

**Salida típica**:

```
FOUND_KEY: 423987
Del espacio le llego algo muy especial
Y lo atrapo y todos sus secretos el sabra
Con superpoderes el cambio y ahora es
Ben 10 (B
```

### (B) Bytes ejemplo + crib

- Bytes ejemplo:

```bash
BYTES="{241,49,35,6,25,159,151,68,69,50,237,92,45,89,154,75,134,19,232,4,94,205,139,65,80,251,236,242,243,42,181,34,218,125,137,244,95,150,190,120}"
```

- Ejecutar:

```bash
mpirun --oversubscribe -np 4 ./bruteforce -mode brute -bytes "$BYTES" -crib " Andres " -bits 28

# o usando crib en hex
mpirun --oversubscribe -np 4 ./bruteforce -mode brute -bytes "$BYTES" -cribhex "20416e6472657320" -bits 28
```

---

## CONSEJOS Y SOLUCIÓN DE PROBLEMAS

- Si el archivo no es múltiplo de 8, usar `-trim` o recortarlo manualmente.  
- Para texto con tildes/caracteres especiales, usar `-cribhex`.  
- Ignorar warnings `OpenSSL DES deprecated` para este ejercicio.  
- Si no se encuentra la clave con `-bits 24`, aumentar a `-bits 28` o más (aumenta coste).  
- Si MPI muestra “not enough slots” en WSL, usar `--oversubscribe` o reducir `-np`.  
- Para verificación, publicar `sha256sum ct.bin` junto con tu post.

---
