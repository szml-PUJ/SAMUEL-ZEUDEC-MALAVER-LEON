#########################################################
#                 Pontificia Universidad Javeriana
#
#       Autores: Juan Camilo Maldonado, Samuel Zeudec, Daniel Plazas
#       Fecha: 05/11/2026
#       Objetivo: Automatizador para compilar el proyecto ISE
#                 (servidorISE, agentePais y generadorCSV)
#########################################################
 
GCC = gcc
FLAGS = -Wall -Wextra -pthread
 
# Se listan todos los programas que se van a compilar
PROGRAMAS = servidorISE agentePais generadorCSV
 
# Compilar todos los programas de una vez
all: $(PROGRAMAS)
 
# El servidor se arma con main.c y servidorISE.c (usa hilos y semaforos)
servidorISE: main.c servidorISE.c servidorISE.h
	$(GCC) $(FLAGS) main.c servidorISE.c -o $@
 
# El agente comparte el protocolo de mensajes con el servidor por servidorISE.h
agentePais: agentePais.c servidorISE.h
	$(GCC) $(FLAGS) $@.c -o $@
 
# El generador de encuestas no depende del servidor
generadorCSV: generadorCSV.c
	$(GCC) $(FLAGS) $@.c -o $@
 
# Se limpian los archivos ejecutables generados
clean:
	rm -f $(PROGRAMAS)
 
.PHONY: all clean