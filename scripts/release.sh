#!/usr/bin/env bash
# 本 fork 的打包版本号工具：算/打 v<YY>.<M>.<D>-<x>（x = 当天第 x 次打包）。
#
# 为什么要脚本：序号 x 只能靠数当天已有的 tag 得出，手敲迟早重号或跳号。
# 为什么年月日不补零：26.09.22-1 这种带前导零的数字标识符不是合法 semver，
# npm 与 Cargo 都会拒（理由见 ADAPTATION.md 第六节）。
# 版本串的唯一出处是 git tag，构建侧从 tag 反读，所以这里只负责产号与落 tag。
set -euo pipefail

usage() {
    cat <<'EOF'
用法：scripts/release.sh [选项]

不带选项时只把当天应使用的版本串（如 26.9.22-1）打到标准输出，过程信息走 stderr。

选项：
  --tag           创建带注释的标签 v<版本串>
  --push          创建后把该标签推送到 origin（含 --tag）
  --allow-dirty   工作区有未提交改动时也继续
  --dry-run       只打印将要执行的 git 命令，不落标签、不推送
  -h, --help      显示本帮助
EOF
}

do_tag=0
do_push=0
allow_dirty=0
dry_run=0

while [ $# -gt 0 ]; do
    case "$1" in
        --tag) do_tag=1 ;;
        --push)
            do_push=1
            do_tag=1
            ;;
        --allow-dirty) allow_dirty=1 ;;
        --dry-run) dry_run=1 ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
    shift
done

cd "$(git rev-parse --show-toplevel)"

# 年月日不补零：%y.%-m.%-d → 26.9.22
today=$(date +%y.%-m.%-d)
prefix="v${today}-"

last=0
while IFS= read -r t; do
    n=${t#"$prefix"}
    [[ $n =~ ^[0-9]+$ ]] || continue
    # 显式按十进制解析：违规的前导零 tag（如 -08）会被 (( )) 当成八进制而报错，
    # 报错即静默跳过，序号就会算少。方案本身要求不补零，这里只是不让手滑打崩脚本。
    n=$((10#${n}))
    if ((n > last)); then
        last=$n
    fi
done < <(git tag -l "${prefix}*")

version="${today}-$((last + 1))"
tag="v${version}"

if ((do_tag)); then
    if ((allow_dirty == 0)) && [ -n "$(git status --porcelain)" ]; then
        echo "工作区有未提交改动，先提交，或加 --allow-dirty" >&2
        exit 1
    fi
    if git rev-parse -q --verify "refs/tags/${tag}" >/dev/null; then
        echo "标签 ${tag} 已存在" >&2
        exit 1
    fi
    if ((dry_run)); then
        echo "+ git tag -a ${tag} -m ${version}" >&2
    else
        git tag -a "$tag" -m "$version"
        echo "已创建标签 ${tag}" >&2
    fi
fi

if ((do_push)); then
    if ((dry_run)); then
        echo "+ git push origin ${tag}" >&2
    else
        git push origin "$tag"
    fi
fi

echo "$version"
