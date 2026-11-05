#pragma once
#ifndef SERVIDOR_ISE_H
#define SERVIDOR_ISE_H

/*
Autores: Juan Camilo Maldonado, Samuel Zeudec, Daniel Plazas
Fichero: servidorISE.h
Objetivo del fichero: Archivo de cabecera que define la interfaz publica del modulo servidorISE.
Contiene la estructura de los mensajes de encuesta, los tipos enumerados y las funciones
principales accesibles desde otros modulos como main.c.
*/

//Aqui va el protocolo de los mensajes

// Enum que define los diferentes tipos de mensajes permitidos en la comunicacion con el servidor
typedef enum {
    MSG_CONEXION,  // Mensaje inicial enviado por el agente para registrar su conexion
    MSG_DATO,      // Mensaje que contiene los datos de una encuesta real (sector y sentimiento)
    MSG_FIN        // Mensaje indicador de que el agente ha finalizado el envio de datos
} TipoMensaje;

#define ISE_MAX_PAIS_TAM   4   // Tamanio maximo para almacenar el codigo del pais y el caracter nulo de cierre
#define ISE_MAX_SECTOR_LEN 16  // Tamaio maximo para la cadena de texto del sector economico

// Estructura utilizada para empaquetar la informacion transmitida a traves del pipe nominal
typedef struct {
    TipoMensaje tipo;          // Tipo de mensaje segun la enumeracion TipoMensaje
    char   pais[ISE_MAX_PAIS_TAM];    // Codigo identificador del pais emisor
    char   sector[ISE_MAX_SECTOR_LEN]; // Nombre descriptivo del sector economico
    int    id_sector;      // Identificador numerico del sector en el rango de 0 a 4
    double sentimiento;    // Valor numerico del sentimiento expresado en la encuesta (0.0 a 100.0)
} MensajeEncuesta;


//Aqui van las funciones publicas

/*
Funcion encargada de procesar y leer los argumentos pasados por linea de comandos (-b, -p, -t, -a),
almacenandolos en las variables internas del modulo. Retorna 0 si el parseo es exitoso o 1 si ocurre un error.
*/
int servidor_parsear_argumentos(int argc, char* argv[]);

/*
Funcion que valida que los parametros configurados previamente se encuentren dentro de los rangos
establecidos por las especificaciones del proyecto. Retorna 0 si son validos o 1 si infringen los rangos.
*/
int servidor_validar_argumentos(void);

/*
Funcion principal que orquesta la ejecucion completa del servidor: crea el pipe nominal, despliega
los hilos de trabajo, controla el tiempo de operacion, consolida los resultados y libera los recursos.
Retorna 0 si finaliza con exito o 1 si surge un error critico.
*/
int servidor_ejecutar(void);

#endif /* SERVIDOR_ISE_H */