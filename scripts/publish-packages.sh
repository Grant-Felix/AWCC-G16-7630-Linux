#!/usr/bin/env bash
# 把 scripts/package.sh 打出来的安装包，挂到三个平台**同名** Release 上。
# 版本串取自最近的日期式 tag（规则见 ADAPTATION.md 第六节），三处保持一致。
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

tag=$(git describe --tags --abbrev=0 --match 'v2*') # 如 v26.9.22-2
dir="${1:-.cache/packages}"
shopt -s nullglob
files=("$dir"/*.deb "$dir"/*.rpm "$dir"/*.pkg.tar.zst)
if [ ${#files[@]} -eq 0 ]; then
  echo "!! $dir 里没有包，先跑 scripts/package.sh"
  exit 1
fi
echo "== 给 $tag 挂 ${#files[@]} 个包 =="
printf '   %s\n' "${files[@]##*/}"

# ── Forgejo（开发主仓）────────────────────────────────────────────────────
token=$(cat "$HOME/.config/forgejo/token")
api="http://127.0.0.1:3000/api/v1/repos/Felix/AWCC-G16-7630-Linux"
rid=$(curl -s -H "Authorization: token $token" "$api/releases/tags/$tag" |
      python3 -c "import json,sys; print(json.load(sys.stdin).get('id',''))")
if [ -n "$rid" ]; then
  for f in "${files[@]}"; do
    curl -s -X POST -H "Authorization: token $token" \
      -F "attachment=@$f" "$api/releases/$rid/assets?name=$(basename "$f")" >/dev/null
    echo "  Forgejo ✓ $(basename "$f")"
  done
else
  echo "  Forgejo：找不到 Release $tag"
fi

# ── GitHub（对外窗口）──────────────────────────────────────────────────────
if command -v gh >/dev/null 2>&1; then
  gh release upload "$tag" "${files[@]}" --clobber --repo Grant-Felix/AWCC-G16-7630-Linux >/dev/null
  echo "  GitHub  ✓ ${#files[@]} 个"
fi

# ── Gitee（国内镜像）──────────────────────────────────────────────────────
if command -v gitee >/dev/null 2>&1; then
  gtoken=$(gitee auth token 2>/dev/null | tr -d '\n')
  grepo="Grant-Felix/AWCC-G16-7630-Linux"
  grid=$(curl -s "https://gitee.com/api/v5/repos/$grepo/releases/tags/$tag?access_token=$gtoken" |
         python3 -c "import json,sys; print(json.load(sys.stdin).get('id',''))")
  if [ -n "$grid" ]; then
    for f in "${files[@]}"; do
      curl -s -X POST \
        "https://gitee.com/api/v5/repos/$grepo/releases/$grid/attach_files?access_token=$gtoken" \
        -F "file=@$f" >/dev/null
      echo "  Gitee   ✓ $(basename "$f")"
    done
  else
    echo "  Gitee：找不到 Release $tag"
  fi
fi

echo "== 完成：三个平台的 Release 上都应能看到上面这些包 =="
