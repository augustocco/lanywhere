// Nombres de un control.nacp. Desde 21.0.0 el bloque de títulos puede ir comprimido (DEFLATE, flag en 0x3215)
// con hasta 32 idiomas en vez de 16; sin descomprimirlo, el nombre sale como caracteres sin sentido.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define NACP_TITLE_SIZE 0x300   // nombre (0x200) + editor (0x100) por idioma
#define NACP_MAX_TITLES 32

// Deja en out (NACP_MAX_TITLES * NACP_TITLE_SIZE bytes) las entradas de título y devuelve cuántas hay (16 o 32),
// o 0 si el NACP no es válido.
int nacp_titles(const unsigned char *nacp, size_t size, unsigned char *out);

// Nombre en el primer idioma con texto de la lista de preferencia (índices de idioma del NACP), si no en
// cualquiera. Devuelve el índice de preferencia usado (-1 si cayó en otro idioma o no hay nombre).
int nacp_name(const unsigned char *nacp, size_t size, const int *preference, int preference_count, char *out,
              size_t out_size);
