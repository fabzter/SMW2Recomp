#include "host_paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #expr);         \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static unsigned failures;

static int is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void test_unset_uses_exe_dir(void) {
  char expected[2048], actual[2048];
  unsetenv("SNESRECOMP_ARTIFACT_DIR");
  snesrecomp_artifact_path_reset();

  int expected_ok =
      snesrecomp_exe_dir_path("ws_input_log.csv", expected, sizeof(expected));
  int actual_ok =
      snesrecomp_artifact_path("ws_input_log.csv", actual, sizeof(actual));
  CHECK(actual_ok == expected_ok);
  if (expected_ok && actual_ok)
    CHECK(strcmp(actual, expected) == 0);
}

static void test_env_dir_and_leaf_parents(const char *root) {
  char artifact_dir[2048], expected[2048], actual[2048], ensured[2048];
  snprintf(artifact_dir, sizeof(artifact_dir), "%s/artifacts", root);
  CHECK(setenv("SNESRECOMP_ARTIFACT_DIR", artifact_dir, 1) == 0);
  snesrecomp_artifact_path_reset();

  snprintf(expected, sizeof(expected), "%s/ws_input_log.csv", artifact_dir);
  CHECK(snesrecomp_artifact_path("ws_input_log.csv", actual, sizeof(actual)) ==
        1);
  CHECK(strcmp(actual, expected) == 0);
  CHECK(is_dir(artifact_dir));

  CHECK(snesrecomp_artifact_dir(ensured, sizeof(ensured)) == 1);
  CHECK(strcmp(ensured, artifact_dir) == 0);
  CHECK(is_dir(ensured));

  snprintf(expected, sizeof(expected), "%s/screenshots/nested/shot.bmp",
           artifact_dir);
  CHECK(snesrecomp_artifact_path("screenshots/nested/shot.bmp", actual,
                                 sizeof(actual)) == 1);
  CHECK(strcmp(actual, expected) == 0);
  snprintf(ensured, sizeof(ensured), "%s/screenshots", artifact_dir);
  CHECK(is_dir(ensured));
  snprintf(ensured, sizeof(ensured), "%s/screenshots/nested", artifact_dir);
  CHECK(is_dir(ensured));
}

static void test_relative_dir_uses_cwd(const char *root) {
  char cwd[2048] = {0}, expected[2048], actual[2048], relative_dir[2048];
  CHECK(chdir(root) == 0);
  CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
  CHECK(setenv("SNESRECOMP_ARTIFACT_DIR", "relative_artifacts", 1) == 0);
  snesrecomp_artifact_path_reset();

  snprintf(expected, sizeof(expected),
           "%s/relative_artifacts/screenshots/x.ppm", cwd);
  CHECK(snesrecomp_artifact_path("screenshots/x.ppm", actual, sizeof(actual)) ==
        1);
  CHECK(strcmp(actual, expected) == 0);
  snprintf(relative_dir, sizeof(relative_dir),
           "%s/relative_artifacts/screenshots", cwd);
  CHECK(is_dir(relative_dir));
}

int main(void) {
  char template[] = "/tmp/snesrecomp-artifact-path-XXXXXX";
  char *root = mkdtemp(template);
  if (!root) {
    fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
    return 1;
  }

  test_unset_uses_exe_dir();
  test_env_dir_and_leaf_parents(root);
  test_relative_dir_uses_cwd(root);

  unsetenv("SNESRECOMP_ARTIFACT_DIR");
  snesrecomp_artifact_path_reset();
  if (chdir("/") != 0)
    failures++;
  char path[2048];
  snprintf(path, sizeof(path), "%s/relative_artifacts/screenshots", root);
  rmdir(path);
  snprintf(path, sizeof(path), "%s/relative_artifacts", root);
  rmdir(path);
  snprintf(path, sizeof(path), "%s/artifacts/screenshots/nested", root);
  rmdir(path);
  snprintf(path, sizeof(path), "%s/artifacts/screenshots", root);
  rmdir(path);
  snprintf(path, sizeof(path), "%s/artifacts", root);
  rmdir(path);
  rmdir(root);

  if (failures) {
    fprintf(stderr, "artifact_path_test: %u failure(s)\n", failures);
    return 1;
  }
  puts("artifact_path_test: all checks passed");
  return 0;
}
