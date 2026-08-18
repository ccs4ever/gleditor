#!/bin/sh
# Check harmony between .editorconfig, .clang-format, .yamlfmt, .yamllint,
# and .mdl_style.rb to ensure formatting and indentation rules do not drift.
set -eu

ROOT_DIR="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOT_DIR"

fail() {
  echo "ERROR (config-harmony): $*" >&2
  exit 1
}

# Helper to read a property from a specific section in .editorconfig
get_editorconfig_val() {
  sec="$1"
  prop="$2"
  awk -F'=' -v target_sec="$sec" -v target_prop="$prop" '
    /^[[:space:]]*\[.*\]/ {
      gsub(/^[[:space:]]*\[|[\]][[:space:]]*$/, "")
      current_sec = $0
      next
    }
    current_sec == target_sec && NF >= 2 {
      key = $1; gsub(/^[[:space:]]+|[[:space:]]+$/, "", key)
      val = $2; gsub(/^[[:space:]]+|[[:space:]]+$/, "", val)
      if (key == target_prop) {
        print val
        exit
      }
    }
  ' .editorconfig
}

# Helper to read a top-level YAML key from a file
get_yaml_top_val() {
  file="$1"
  prop="$2"
  awk -F':' -v target_prop="$prop" '
    $1 ~ "^[[:space:]]*" target_prop "$" {
      val = $2
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", val)
      print val
      exit
    }
  ' "$file"
}

[ -f .editorconfig ] || fail ".editorconfig not found"
[ -f .clang-format ] || fail ".clang-format not found"
[ -f .yamlfmt ] || fail ".yamlfmt not found"
[ -f .yamllint ] || fail ".yamllint not found"
[ -f .mdl_style.rb ] || fail ".mdl_style.rb not found"

# 1. Check C++ and GLSL settings between .editorconfig and .clang-format
cf_indent=$(get_yaml_top_val .clang-format "IndentWidth")
cf_use_tab=$(get_yaml_top_val .clang-format "UseTab")
cf_col_limit=$(get_yaml_top_val .clang-format "ColumnLimit")

ec_cpp_indent=$(get_editorconfig_val "*.{cpp,hpp,h,glsl}" "indent_size")
ec_cpp_style=$(get_editorconfig_val "*.{cpp,hpp,h,glsl}" "indent_style")
ec_cpp_col=$(get_editorconfig_val "*.{cpp,hpp,h,glsl}" "max_line_length")
ec_cpp_eol=$(get_editorconfig_val "*" "end_of_line")

[ -n "$cf_indent" ] || fail "Unable to read IndentWidth from .clang-format"
[ -n "$ec_cpp_indent" ] || fail "Unable to read indent_size for *.{cpp,hpp,h,glsl} from .editorconfig"

if [ "$cf_indent" != "$ec_cpp_indent" ]; then
  fail "C++ indent mismatch: .clang-format IndentWidth ($cf_indent) != .editorconfig indent_size ($ec_cpp_indent)"
fi

if [ "$cf_use_tab" = "Never" ] && [ "$ec_cpp_style" != "space" ]; then
  fail "C++ tab style mismatch: .clang-format UseTab is Never but .editorconfig indent_style is $ec_cpp_style"
fi

if [ -n "$cf_col_limit" ] && [ -n "$ec_cpp_col" ] && [ "$cf_col_limit" != "$ec_cpp_col" ]; then
  fail "C++ column limit mismatch: .clang-format ColumnLimit ($cf_col_limit) != .editorconfig max_line_length ($ec_cpp_col)"
fi

if [ "$ec_cpp_eol" != "lf" ]; then
  fail "Global end_of_line in .editorconfig is '$ec_cpp_eol', expected 'lf'"
fi

# 2. Check YAML settings between .editorconfig, .yamlfmt, and .yamllint
yf_indent=$(awk -F':' '/^[[:space:]]*indent:[[:space:]]*/ { gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit }' .yamlfmt)
yl_col=$(awk '/line-length:/ { found=1; next } found && /max:/ { gsub(/[^0-9]/, ""); print; exit }' .yamllint)

ec_yaml_indent=$(get_editorconfig_val "*.{yml,yaml}" "indent_size")
ec_yaml_col=$(get_editorconfig_val "*.{yml,yaml}" "max_line_length")

if [ -n "$yf_indent" ] && [ "$yf_indent" != "$ec_yaml_indent" ]; then
  fail "YAML indent mismatch: .yamlfmt indent ($yf_indent) != .editorconfig indent_size ($ec_yaml_indent)"
fi

if [ -n "$yl_col" ] && [ "$yl_col" != "$ec_yaml_col" ]; then
  fail "YAML column limit mismatch: .yamllint line-length max ($yl_col) != .editorconfig max_line_length ($ec_yaml_col)"
fi

# 3. Check Markdown settings between .editorconfig and .mdl_style.rb
mdl_col=$(awk -F'=>' '/MD013/ { for(i=1;i<=NF;i++) if($i ~ /line_length/) { split($(i+1), a, ","); gsub(/[^0-9]/, "", a[1]); print a[1]; exit } }' .mdl_style.rb)
ec_md_col=$(get_editorconfig_val "*.md" "max_line_length")
ec_md_trim=$(get_editorconfig_val "*.md" "trim_trailing_whitespace")

if [ -n "$mdl_col" ] && [ "$mdl_col" != "$ec_md_col" ]; then
  fail "Markdown column limit mismatch: .mdl_style.rb line_length ($mdl_col) != .editorconfig max_line_length ($ec_md_col)"
fi

if [ "$ec_md_trim" != "false" ]; then
  fail "Markdown trim_trailing_whitespace in .editorconfig must be 'false' to preserve CommonMark hard breaks"
fi

# 4. Check Makefile and debian/rules tab indentation
ec_make_style=$(get_editorconfig_val "Makefile" "indent_style")
ec_deb_style=$(get_editorconfig_val "debian/rules" "indent_style")

if [ "$ec_make_style" != "tab" ]; then
  fail "Makefile indent_style in .editorconfig must be 'tab'"
fi

if [ "$ec_deb_style" != "tab" ]; then
  fail "debian/rules indent_style in .editorconfig must be 'tab'"
fi

# 5. Check Shell script indentation
ec_sh_indent=$(get_editorconfig_val "*.sh" "indent_size")
if [ "$ec_sh_indent" != "2" ]; then
  fail "Shell script indent_size in .editorconfig must be 2 (matching shfmt -i 2)"
fi

echo "config-harmony: .editorconfig, .clang-format, .yamlfmt, .yamllint, and .mdl_style.rb are in harmony"
