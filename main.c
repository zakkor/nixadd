#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <libgen.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "strutil.h"
#include "dynamic_read.h"
#include "rename.h"

//#define DEBUG

#define CMD "nixos-rebuild"
#define ARG "switch"

#define BS 256
#define RMAX (2 << 30)

#define DNA_SUFF "/.config/.dna"
#define CFG_SUFF "/.config"
#define CFG_DFLT "/etc/nixos/configuration.nix"
#define TMP_SUFF ".dnatmp"
#define OLD_SUFF ".dna"

const char *usage = "usage: %s [OPTIONS] ... [PKG] ...\n"
    "\t-c Specify nix configuration file for this use only\n"
    "\t-C Set persistent nix configuration file location\n"
    "\t-t Text only mode. Do not call `nixos-rebuild`\n"
    "\t-q Only print `nixos-rebuild` output if it fails\n" "\t-h Displays this message\n";

//extern char **environ;

int main(int argc, char **argv)
{
	int status = 0;
	int quiet = 0;
	int io_p[2];
	int t_mode = 0;		//text only mode. do not nixos-rebuild
	int C_mode = 0;
	int c_sat = 0;
	int eno;
	int exit_usage = 0;
	int opt;
#ifdef DEBUG
	char *_config_path = "./configuration.nix";
	fprintf(stderr, "%s", "DEBUG BUILD\n");
#else
	char *_config_path = CFG_DFLT;	//default config location for NixOS
#endif
	char *C_str = NULL;
	char dna_cfg_path[PATH_MAX];	//the contents of ~/.config/.dna

	while ((opt = getopt(argc, argv, "qtC:hc:")) != -1) {
		switch (opt) {
		case 'q':
			quiet = 1;
			break;
		case 't':
			t_mode = 1;
			break;
		case 'C':
			C_str = optarg;
			C_mode = 1;
			break;
		case 'c':
			c_sat = 1;
			_config_path = optarg;
			break;
		case '?':
			exit(EXIT_FAILURE);	//getopt errors for us
			break;
		case 'h':
		default:
			exit_usage = 1;
		}
	}
	if (exit_usage || (optind == argc && !C_mode)) {
		fprintf(stderr, usage, argv[0]);
		exit(EXIT_FAILURE);
	}

	char *home_path = getenv("HOME");
	const size_t hp_len = strlen(home_path);
	char dna_path[hp_len + sizeof(DNA_SUFF)];
	sprintf(dna_path, "%s%s", home_path, DNA_SUFF);

	if (C_mode) {
		errno = 0;
		char *C_path = realpath(C_str, NULL);	//let realpath alloc
		eno = errno;
		if (eno) {
			if (C_path == NULL) {
				fprintf(stderr, "Couldn't use \"%s\": %s\n", C_str, strerror(eno));
				exit(EXIT_FAILURE);
			}
			printf("Warning: %s didn't resolve correctly\n", C_str);
		} else {
			C_str = C_path;
		}

		struct stat st = { 0 };
		char dotcfg_path[hp_len + sizeof(CFG_SUFF)];
		sprintf(dotcfg_path, "%s%s", home_path, CFG_SUFF);

		errno = 0;
		DIR *d = opendir(dotcfg_path);
		if (d) {
			closedir(d);
		} else {
			errno = 0;
			stat(home_path, &st);
			eno = errno;
			if (eno) {
				fprintf(stderr,
					"%s doesn't exist and couldn't be created when trying to enter %s: %s\n",
					dotcfg_path, home_path, strerror(eno));
				goto err_pu;
			}
			errno = 0;
			mkdir(dotcfg_path, st.st_mode);
			eno = errno;
			if (eno) {
				fprintf(stderr, "%s doesn't exist and couldn't be created: %s\n", dotcfg_path,
					strerror(eno));
				goto err_pu;
			}
		}
		printf("%s was created.\n", dotcfg_path);
		errno = 0;
		FILE *dna = fopen(dna_path, "w");
		eno = errno;
		if (eno) {
			fprintf(stderr, "T: Error opening %s: %s\n", dna_path, strerror(eno));
			goto err_pu;
		}
		if (fputs(C_str, dna) < 0) {
			fprintf(stderr, "Couldn't write to %s\n", dna_path);
			goto err_op;
		}
		fclose(dna);
		printf("Set %s as default config file.\nIn %s\n", C_str, dna_path);
		free(C_path);
		return 0;
 err_op:
		fclose(dna);
 err_pu:
		free(C_path);
		exit(EXIT_FAILURE);
	}
	if (!c_sat) {
		errno = 0;
		FILE *dna = fopen(dna_path, "r");
		eno = errno;
		if (eno && eno != ENOENT) {
			fprintf(stderr, "Error: couldn't open ~" DNA_SUFF ": %s\n", strerror(eno));
			//fail or continue with default?
			exit(EXIT_FAILURE);
		}
		if (eno == ENOENT) {	// if ~/.config/.dna doesn't exist then notify user instead of failing
			puts("Note: you haven't set a location for your config yet. Use -C");
			puts("Defaulting to: " CFG_DFLT);
		} else {
			if (fgets(dna_cfg_path, PATH_MAX, dna) == NULL) {	//get the first line of .dna only
				fprintf(stderr, "Error reading from %s\n", dna_path);
				fclose(dna);
				exit(EXIT_FAILURE);
			}
			_config_path = dna_cfg_path;
		}
		if (dna) {
			fclose(dna);
		}

	}

	errno = 0;
	char *cfg_full_path = realpath(_config_path, NULL);	//let realpath alloc
	eno = errno;
	//propagate both errors, cfg_full_path might be NULL for fopen but it's ok.

	int enorp = eno;
	errno = 0;
	FILE *fp = fopen(cfg_full_path, "r");
	eno = errno;
	//use _config_path instead of path_buf in failure in case it was realpath that failed

	if (eno || enorp) {
		if (eno == EACCES || enorp == EACCES) {
			fprintf(stderr, "Permission denied for %s. (Are you sudo?)\n", _config_path);
		} else {
			fprintf(stderr, "Error opening file %s\n", _config_path);
		}
		if (fp) {
			fclose(fp);
		}
		if (cfg_full_path) {
			free(cfg_full_path);
		}
		exit(EXIT_FAILURE);
	}

	const size_t cfp_len = strlen(cfg_full_path);
	char temp_path[cfp_len + sizeof(TMP_SUFF)];
	char backup_file_path[cfp_len + sizeof(OLD_SUFF)];

	sprintf(backup_file_path, "%s%s", cfg_full_path, OLD_SUFF);
	sprintf(temp_path, "%s%s", cfg_full_path, TMP_SUFF);

	errno = 0;
	FILE *dfp = fopen(backup_file_path, "w");
	eno = errno;
	if (eno) {
		fprintf(stderr, "Error with %s: %s\n", backup_file_path, strerror(eno));
		fclose(fp);
		exit(EXIT_FAILURE);
	}

	if (insertpkgs(&argv[optind], argc - optind, fp, dfp)) {
		fprintf(stderr, "%s did not contain " MARKER "\n", cfg_full_path);
		goto errf;
	}
	swap_names(backup_file_path, cfg_full_path, temp_path);
	fclose(fp);
	fclose(dfp);
	printf("Successfully edited %s\n", cfg_full_path);

	if (!t_mode) {
		if (quiet) {	//ironically print more
			puts("Running " CMD "\n");	//so the user knows at least something's happening
		}
		if (pipe(io_p) == -1) {
			exit(EXIT_FAILURE);
		}

		pid_t pid = fork();

		if (pid == -1) {
			perror("Fork:");
			goto errc;
		} else if (pid > 0) {	//parent
			if (quiet) {
				close(io_p[1]);
				int bread = 0;
				int pf;
				char *buf = d_read(io_p[0], &bread, BS, RMAX, &pf);
				if (buf == NULL) {
					fprintf(stderr, "Lost output from " CMD " " ARG "\n");
				}
				waitpid(pid, &status, 0);
				if (status) {
					printf("%.*s\n", bread, buf);
					free(buf);
				}
				if (pf) {
					fprintf(stderr, "Lost some output from " CMD " " ARG "\n");
				}
			} else {
				waitpid(pid, &status, 0);
			}
			puts("Done.\n");

		} else {
			if (quiet) {
				dup2(io_p[1], STDOUT_FILENO);
				dup2(io_p[1], STDERR_FILENO);
				close(io_p[0]);
				close(io_p[1]);
			}

			char *const _argv[] = { CMD, ARG, "--show-trace", NULL };

			if (execvpe(CMD, _argv, environ) < 0) {
				perror("execvpe:");
				goto errc;
			}
		}
	}
	if (status) {
		fprintf(stderr, "%s: " CMD " failed.\n config has been reverted\n", argv[0]);
		swap_names(backup_file_path, cfg_full_path, temp_path);
	}
	free(cfg_full_path);
	return status;		//guaranteed 0 if (t) otherwise exit status of exec
 errf:
	fclose(fp);
	fclose(dfp);
 errc:
	free(cfg_full_path);
	exit(EXIT_FAILURE);
}
