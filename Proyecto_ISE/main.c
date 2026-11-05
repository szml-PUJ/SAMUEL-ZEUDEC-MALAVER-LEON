/*
Autores: Juan Camilo Maldonado, Samuel Zeudec, Daniel Plazas
Fichero: main.c
Objetivo del fichero: Punto de entrada del servidor ISE. Solo llama en orden a las funciones
publicas de servidorISE.h: leer los argumentos, validarlos y ejecutar el servidor.
*/

#include "servidorISE.h" //Interfaz publica del servidor (parsear, validar y ejecutar)

int main(int argc, char* argv[]) { //Recibe los argumentos de la linea de comandos (-b, -p, -t, -a)
    if (servidor_parsear_argumentos(argc, argv) != 0) //Lee las banderas (el propio servidor muestra el uso si hay error)
        return 1; //Termina con error
    if (servidor_validar_argumentos() != 0) //Comprueba que los valores esten dentro de los rangos del enunciado
        return 1; //Termina con error
    return servidor_ejecutar(); //Corre el servidor y retorna 0 si termino bien o 1 si hubo un error
}
