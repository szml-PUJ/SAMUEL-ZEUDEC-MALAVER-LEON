#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>

/*********************************************************************************
 * Pontificia Universidad Javeriana
 *
 * Autores: Daniel Plazas, Juan Camilo Maldonado, Samuel Zeudec
 * Fecha: 4 de Noviembre 2026
 * Materia: Sistemas Operativos
 * Proyecto: Indicador de Sentimiento Economico (ISE)
 * Fichero: medir_tiempos.c
 * Tema: Medicion de tiempos: automatiza las pruebas de rendimiento
 * Objetivo del fichero: Medir cuanto tarda el sistema en recibir y procesar las
 * encuestas al variar su cantidad, el tamano del buffer y el numero de agentes.
 ********************************************************************************/

#define REPETICIONES 3
#define TIEMPO_SERVIDOR 30
#define PIPE "ise_fifo_medicion"
#define CARPETA "medicion_tmp"
#define RESULTADOS "resultados_tiempos.csv"

const char *PAISES[] = {"FR", "ES", "IT"};

// Obtener tiempo monotónico de alta precisión en segundos
double obtener_tiempo() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// Limpiar archivos y procesos temporales
void limpiar() {
    char cmd[256];
    int ret;
    snprintf(cmd, sizeof(cmd), "pkill -f 'servidorISE .* -p %s' 2>/dev/null", PIPE);
    ret = system(cmd);
    (void)ret;
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", CARPETA, PIPE);
    ret = system(cmd);
    (void)ret;
}

// Generar subconjunto usando un comando similar al script original
void crear_subconjunto(int cantidad, const char *pais) {
    char archivo[256], original[256], cmd[1024];
    snprintf(archivo, sizeof(archivo), "%s/n_%s_%d.csv", CARPETA, pais, cantidad);
    
    // Verificar si ya existe
    if (access(archivo, F_OK) != -1) return;

    snprintf(original, sizeof(original), "%s/encuestas_%s.csv", CARPETA, pais);
    snprintf(cmd, sizeof(cmd), 
             "{ head -n 1 %s; tail -n +2 %s | awk 'BEGIN { srand(2026) } { print rand() \"\\t\" $0 }' | sort -n | head -n %d | cut -f2-; } > %s", 
             original, original, cantidad, archivo);
    int ret = system(cmd);
    (void)ret;
}

// Estructura para almacenar resultados de una medición
typedef struct {
    double t_agente;
    double t_total;
    char estado[32];
} ResultadoMedicion;

ResultadoMedicion medir_una_vez(int cantidad, int buffer, int agentes) {
    ResultadoMedicion res;
    char salida_servidor[256], salida_agente[256], fin_agente_archivo[256];
    snprintf(salida_servidor, sizeof(salida_servidor), "%s/salida_servidor.txt", CARPETA);
    
    unlink(salida_servidor);
    unlink(PIPE);

    for (int i = 0; i < agentes; i++) {
        crear_subconjunto(cantidad, PAISES[i]);
    }

    // 1. Lanzar servidorISE usando fork() y execvp()
    pid_t pid_serv = fork();
    if (pid_serv == 0) {
        // Proceso hijo: Servidor
        int fd_out = open(salida_servidor, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(fd_out, STDOUT_FILENO);
        dup2(fd_out, STDERR_FILENO);
        close(fd_out);

        char buf_str[16], time_str[16], ag_str[16];
        snprintf(buf_str, sizeof(buf_str), "%d", buffer);
        snprintf(time_str, sizeof(time_str), "%d", TIEMPO_SERVIDOR);
        snprintf(ag_str, sizeof(ag_str), "%d", agentes);

        char *args[] = {"stdbuf", "-oL", "./servidorISE", "-b", buf_str, "-p", PIPE, "-t", time_str, "-a", ag_str, NULL};
        execvp("stdbuf", args);
        exit(1);
    }

    // Esperar a que el servidor esté listo leyendo el archivo de salida
    int espera = 0;
    FILE *f_serv = NULL;
    int listo = 0;
    while (espera < 100) {
        usleep(50000); // 50 ms
        f_serv = fopen(salida_servidor, "r");
        if (f_serv) {
            char linea[512];
            while (fgets(linea, sizeof(linea), f_serv)) {
                if (strstr(linea, "Esperando conexiones")) {
                    listo = 1;
                    break;
                }
            }
            fclose(f_serv);
            if (listo) break;
        }
        espera++;
    }

    if (!listo) {
        kill(pid_serv, SIGTERM);
        waitpid(pid_serv, NULL, 0);
        res.t_agente = 0; res.t_total = 0;
        strcpy(res.estado, "ERROR_SERVIDOR");
        return res;
    }

    usleep(300000); // 300 ms margen

    // 2. Lanzar agentes en paralelo con fork()
    double inicio = obtener_tiempo();
    pid_t pids_agentes[3];

    for (int i = 0; i < agentes; i++) {
        snprintf(salida_agente, sizeof(salida_agente), "%s/salida_agente_%d.txt", CARPETA, i);
        snprintf(fin_agente_archivo, sizeof(fin_agente_archivo), "%s/fin_agente_%d.txt", CARPETA, i);
        
        pids_agentes[i] = fork();
        if (pids_agentes[i] == 0) {
            // Proceso hijo: Agente
            int fd_ag = open(salida_agente, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(fd_ag, STDOUT_FILENO);
            dup2(fd_ag, STDERR_FILENO);
            close(fd_ag);

            char n_path[256];
            snprintf(n_path, sizeof(n_path), "%s/n_%s_%d.csv", CARPETA, PAISES[i], cantidad);

            char *args[] = {"./agentePais", "-n", n_path, "-p", PIPE, NULL};
            execvp("./agentePais", args);
            exit(1);
        }
    }

    // Esperar a que terminen los agentes y registrar su marca de tiempo de fin
    for (int i = 0; i < agentes; i++) {
        waitpid(pids_agentes[i], NULL, 0);
        double fin_i = obtener_tiempo();
        snprintf(fin_agente_archivo, sizeof(fin_agente_archivo), "%s/fin_agente_%d.txt", CARPETA, i);
        FILE *f_fin = fopen(fin_agente_archivo, "w");
        if (f_fin) {
            fprintf(f_fin, "%.6f\n", fin_i);
            fclose(f_fin);
        }
    }

    // 3. Esperar reporte parcial del servidor (máximo 20 segundos)
    double limite = obtener_tiempo() + 20.0;
    int reporte_ok = 0;
    while (obtener_tiempo() < limite) {
        usleep(1000);
        f_serv = fopen(salida_servidor, "r");
        if (f_serv) {
            int cuenta_reportes = 0;
            char linea[512];
            while (fgets(linea, sizeof(linea), f_serv)) {
                if (strstr(linea, "Reporte parcial")) {
                    cuenta_reportes++;
                }
            }
            fclose(f_serv);
            if (cuenta_reportes >= agentes) {
                reporte_ok = 1;
                break;
            }
        }
    }

    double fin_total = obtener_tiempo();

    if (!reporte_ok) {
        kill(pid_serv, SIGTERM);
        waitpid(pid_serv, NULL, 0);
        res.t_agente = 0; res.t_total = 0;
        strcpy(res.estado, "SIN_REPORTE");
        return res;
    }

    // Verificar estado OK revisando si recibió todas las encuestas
    strcpy(res.estado, "OK");
    f_serv = fopen(salida_servidor, "r");
    if (f_serv) {
        char contenido[16384] = {0};
        size_t bytes_leidos = fread(contenido, 1, sizeof(contenido) - 1, f_serv);
        (void)bytes_leidos;
        fclose(f_serv);

        for (int i = 0; i < agentes; i++) {
            char buscado[128];
            snprintf(buscado, sizeof(buscado), "Total %s: %d encuestas recibidas", PAISES[i], cantidad);
            if (!strstr(contenido, buscado)) {
                strcpy(res.estado, "TOTAL_INCORRECTO");
            }
        }
    }

    // Calcular tiempo máximo de agente
    double fin_agente_max = 0;
    for (int i = 0; i < agentes; i++) {
        snprintf(fin_agente_archivo, sizeof(fin_agente_archivo), "%s/fin_agente_%d.txt", CARPETA, i);
        FILE *f_fin = fopen(fin_agente_archivo, "r");
        if (f_fin) {
            double fin_i;
            int scanned = fscanf(f_fin, "%lf", &fin_i);
            (void)scanned;
            fclose(f_fin);
            if (fin_i > fin_agente_max) fin_agente_max = fin_i;
        }
    }

    // Detener servidor de forma limpia
    kill(pid_serv, SIGTERM);
    waitpid(pid_serv, NULL, 0);
    unlink(PIPE);

    res.t_agente = fin_agente_max - inicio;
    res.t_total = fin_total - inicio;
    return res;
}

ResultadoMedicion medir_promedio(int cantidad, int buffer, int agentes) {
    double suma_agente = 0, suma_total = 0;
    char estado_final[32] = "OK";

    for (int r = 0; r < REPETICIONES; r++) {
        ResultadoMedicion m = medir_una_vez(cantidad, buffer, agentes);
        suma_agente += m.t_agente;
        suma_total += m.t_total;
        if (strcmp(m.estado, "OK") != 0) {
            strcpy(estado_final, m.estado);
        }
    }

    ResultadoMedicion prom;
    prom.t_agente = suma_agente / REPETICIONES;
    prom.t_total = suma_total / REPETICIONES;
    strcpy(prom.estado, estado_final);
    return prom;
}

int main() {
    // Comprobar que existan los programas necesarios
    const char *programas[] = {"servidorISE", "agentePais", "generadorCSV"};
    for (int i = 0; i < 3; i++) {
        char ruta[256];
        snprintf(ruta, sizeof(ruta), "./%s", programas[i]);
        if (access(ruta, X_OK) != 0) {
            printf("Error: falta ./%s. Ejecuta make primero.\n", programas[i]);
            return 1;
        }
    }

    limpiar();
    mkdir(CARPETA, 0777);

    printf("Generando CSV base (ES, FR e IT al 100 %%)...\n\n");
    for (int i = 0; i < 3; i++) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "cd %s && ../generadorCSV -p %s -e 100 > /dev/null", CARPETA, PAISES[i]);
        int ret = system(cmd);
        (void)ret;
    }

    FILE *f_res = fopen(RESULTADOS, "w");
    fprintf(f_res, "experimento,encuestas_por_agente,buffer,agentes,encuestas_totales,tiempo_agente_s,tiempo_total_s,encuestas_por_s,estado\n");

    // Experimento 1
    printf("Experimento 1: tiempo segun la cantidad de encuestas (1 agente, -b 10)\n");
    printf("%-12s %-18s %-16s %-22s %s\n", "Encuestas", "Tiempo agente (s)", "Tiempo total (s)", "Rendimiento (enc/s)", "Estado");
    
    int exp1_n[] = {1000, 5000, 6000, 10000, 20000, 36640};
    for (int i = 0; i < 6; i++) {
        int n = exp1_n[i];
        ResultadoMedicion m = medir_promedio(n, 10, 1);
        int rend = (m.t_total > 0) ? (int)(n / m.t_total) : 0;
        printf("%-12d %-18.3f %-16.3f %-22d %s\n", n, m.t_agente, m.t_total, rend, m.estado);
        fprintf(f_res, "1,%d,10,1,%d,%.3f,%.3f,%d,%s\n", n, n, m.t_agente, m.t_total, rend, m.estado);
    }
    printf("\n");

    // Experimento 2
    printf("Experimento 2: efecto del tamano del buffer (1 agente, 6000 encuestas)\n");
    printf("%-12s %-18s %-16s %s\n", "Buffer -b", "Tiempo agente (s)", "Tiempo total (s)", "Estado");
    
    int exp2_b[] = {1, 10, 100, 1000};
    for (int i = 0; i < 4; i++) {
        int b = exp2_b[i];
        ResultadoMedicion m = medir_promedio(6000, b, 1);
        printf("%-12d %-18.3f %-16.3f %s\n", b, m.t_agente, m.t_total, m.estado);
        fprintf(f_res, "2,6000,%d,1,6000,%.3f,%.3f,,%s\n", b, m.t_agente, m.t_total, m.estado);
    }
    printf("\n");

    // Experimento 3
    printf("Experimento 3: efecto de varios agentes a la vez (5000 encuestas por agente, -b 10)\n");
    printf("%-10s %-20s %-16s %-22s %s\n", "Agentes", "Encuestas totales", "Tiempo total (s)", "Rendimiento (enc/s)", "Estado");
    
    for (int a = 1; a <= 3; a++) {
        int total = 5000 * a;
        ResultadoMedicion m = medir_promedio(5000, 10, a);
        int rend = (m.t_total > 0) ? (int)(total / m.t_total) : 0;
        printf("%-10d %-20d %-16.3f %-22d %s\n", a, total, m.t_total, rend, m.estado);
        fprintf(f_res, "3,5000,10,%d,%d,%.3f,%.3f,%d,%s\n", a, total, m.t_agente, m.t_total, rend, m.estado);
    }
    printf("\n");

    fclose(f_res);
    printf("Listo. Resultados guardados en %s\n", RESULTADOS);
    printf("Si alguna fila dice un estado distinto de OK, esa medicion no es valida: revisa el agente o el servidor.\n");

    limpiar();
    return 0;
}