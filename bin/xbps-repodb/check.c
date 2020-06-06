#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>

#include <xbps.h>

#include "defs.h"

struct arch {
	char arch[64];
	struct arch *next;
	struct xbps_repo *repo, *stage;
};

struct repo {
	char path[PATH_MAX];
	struct repo *next;
	struct dirent **namelist;
	int nnames;
	struct arch *archs;
};

static struct repo *repos;

static void
add_repo(struct xbps_handle *xhp, const char *path)
{
	struct repo *repo = calloc(1, sizeof (struct repo));
	if (repo == NULL) {
		perror("calloc");
		exit(EXIT_FAILURE);
	}
	xbps_strlcpy(repo->path, path, sizeof repo->path);
	repo->namelist = NULL;
	repo->nnames = 0;
	repo->archs = NULL;
	repo->next = repos;
	repos = repo;
	xbps_dbg_printf(xhp, "Scanning repository: %s\n", path);

	repo->nnames = scandir(path, &repo->namelist, NULL, NULL);
	if (repo->nnames == -1) {
		perror("scandir");
		exit(EXIT_FAILURE);
	}

	for (int i = 0; i < repo->nnames; i++) {
		const char *name = repo->namelist[i]->d_name, *d;
		if (*name == '.')
			continue;
		if ((d = strrchr(name, '-')) == NULL)
			continue;
		if (strcmp(d+1, "repodata") == 0) {
			struct arch *arch = calloc(1, sizeof (struct arch));
			if (arch == NULL) {
				perror("calloc");
				exit(1);
			}
			if ((size_t)(d-name) >= sizeof arch->arch) {
				xbps_error_printf("invalid repodata: %s\n", name);
				exit(1);
			}
			strncpy(arch->arch, name, d-name);

			// skip unwanted arches
			if (1 && strcmp(arch->arch, "x86_64") != 0) {
				free(arch);
				continue;
			}

			arch->next = repo->archs;
			repo->archs = arch;
			xbps_dbg_printf(xhp, "  found architecture: %s\n", arch->arch);

			xhp->target_arch = arch->arch;
			arch->repo = xbps_repo_public_open(xhp, path);
			if (arch->repo == NULL) {
				xbps_error_printf("Failed to read repodata: %s",
				    strerror(errno));
				exit(1);
			}
			arch->stage = xbps_repo_stage_open(xhp, path);
			if (arch->repo == NULL && errno != ENOENT) {
				xbps_error_printf("Failed to read stagedata: %s",
				    strerror(errno));
				exit(1);
			}
		}
	}
}

/*
 * For each pkg in the stage:
 *  For each dep of the pkg:
 *   check satisfied by stage
 *   check satisfied by public
 */

/*
 * Save list of pkg in the stage
 * Commit stage to public
 * For each pkg in list:
 *  For each dep of the pkg:
 *   For each repo:
 *    check satisfied
 */

static int
check_deps(struct repo *repo_) {
	xbps_dictionary_t index = repo_->archs->repo->idx;
	xbps_object_iterator_t iter;
	xbps_dictionary_keysym_t keysym;
	xbps_array_t deps;

	printf("\nChecking %s:\n", repo_->path);
	
	iter = xbps_dictionary_iterator(index);
	while ((keysym = xbps_object_iterator_next(iter))) {
		xbps_dictionary_t pkgd = xbps_dictionary_get_keysym(index, keysym);
		const char *pkgver = NULL;

		xbps_dictionary_get_cstring_nocopy(pkgd, "pkgver", &pkgver);
		deps = xbps_dictionary_get(pkgd, "run_depends");

		for (unsigned int i=0; i < xbps_array_count(deps); i++) {
			const char *dep = NULL;
			xbps_dictionary_t found = NULL;

			xbps_array_get_cstring_nocopy(deps, i, &dep);
			for (struct repo *repo = repos; repo; repo = repo->next) {
				if ((found = xbps_repo_get_pkg(repo->archs->repo, dep)))
					break;
				if ((found = xbps_repo_get_virtualpkg(repo->archs->repo, dep)))
					break;
			}

			if (!found)
				printf("%s: not found (required by %s)\n", dep, pkgver);
		}
	}
	xbps_object_iterator_release(iter);
	return 0;
}

static void
check_shlibs(void) {
	xbps_dictionary_t shlibs;
	xbps_object_iterator_t iter;
	xbps_dictionary_keysym_t keysym;

	printf("\nSHLIBS CHECKING:\n");
	shlibs = xbps_dictionary_create();

	for (struct repo *repo = repos; repo; repo = repo->next) {
		xbps_dictionary_t index = repo->archs->repo->idx;
		iter = xbps_dictionary_iterator(index);
		while ((keysym = xbps_object_iterator_next(iter))) {
			xbps_dictionary_t pkgd = xbps_dictionary_get_keysym(index, keysym);
			xbps_array_t pkgshlibs = xbps_dictionary_get(pkgd, "shlib-provides");
			for (unsigned int i = 0; i < xbps_array_count(pkgshlibs); i++) {
				const char *shlib = NULL;
				xbps_array_get_cstring_nocopy(pkgshlibs, i, &shlib);
				xbps_dictionary_set_cstring(shlibs, shlib, "x");
			}
		}
		xbps_object_iterator_release(iter);
	}

	for (struct repo *repo = repos; repo; repo = repo->next) {
		xbps_dictionary_t index = repo->archs->repo->idx;
		iter = xbps_dictionary_iterator(index);
		while ((keysym = xbps_object_iterator_next(iter))) {
			xbps_dictionary_t pkgd = xbps_dictionary_get_keysym(index, keysym);
			xbps_array_t pkgshlibs = xbps_dictionary_get(pkgd, "shlib-requires");
			const char *pkgver = NULL;

			xbps_dictionary_get_cstring_nocopy(pkgd, "pkgver", &pkgver);
			for (unsigned int i = 0; i < xbps_array_count(pkgshlibs); i++) {
				const char *shlib = NULL;
				xbps_array_get_cstring_nocopy(pkgshlibs, i, &shlib);
				if (!xbps_dictionary_get(shlibs, shlib))
					printf("%s: not found (required by %s)\n", shlib, pkgver);
			}
		}
		xbps_object_iterator_release(iter);
	}
}

int
check(struct xbps_handle *xhp, int argc, char *argv[])
{
	struct repo *rep;
	for (int i = 0; i < argc; i++)
		add_repo(xhp, argv[i]);
	rep = repos;
	printf("%p\n", rep);
	for (struct repo *repo = repos; repo; repo = repo->next)
		check_deps(repo);
	check_shlibs();
	return 0;
}
