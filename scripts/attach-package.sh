#!/usr/bin/env bash
# 把打好的安装包挂到 Release 上（默认挂 Forgejo，也就是开发主仓）。
#
# CI（.forgejo/workflows/package.yml）里用 Actions 自动令牌，本地用 ~/.config/forgejo/token。
# 对应 tag 的 Release 不存在就先创建，这样 tag 一推、包自动就上去了。
#
# 环境变量：
#   TOKEN         覆盖令牌（CI 里传 secrets.GITHUB_TOKEN）
#   FORGEJO_API   覆盖 API 根（CI 里传 http://server:3000/api/v1）
#   REPO          覆盖仓库（默认 Felix/AWCC-G16-7630-Linux）
#   RELEASE_TAG   覆盖 tag（默认取最近一个日期式 tag）
set -euo pipefail

repo="${REPO:-Felix/AWCC-G16-7630-Linux}"
api="${FORGEJO_API:-http://127.0.0.1:3000/api/v1}"
tag="${RELEASE_TAG:-$(git describe --tags --abbrev=0 --match 'v2*')}"
token="${TOKEN:-$(cat "$HOME/.config/forgejo/token")}"

if [ $# -eq 0 ]; then
    echo "用法：$0 <包文件>…" >&2
    exit 2
fi

# Release 不存在就建（标题与 tag 同串）
release_id=$(curl -s -H "Authorization: token $token" "$api/repos/$repo/releases/tags/$tag" |
             python3 -c "import json,sys; print(json.load(sys.stdin).get('id',''))")
if [ -z "$release_id" ]; then
    release_id=$(curl -s -X POST -H "Authorization: token $token" -H "Content-Type: application/json" \
        -d "{\"tag_name\":\"$tag\",\"name\":\"$tag\",\"body\":\"见 CHANGELOG.md\"}" \
        "$api/repos/$repo/releases" | python3 -c "import json,sys; print(json.load(sys.stdin)['id'])")
    echo "已创建 Release $tag (id=$release_id)"
fi

for f in "$@"; do
    name=$(basename "$f")
    # 先删同名资产：重跑同一个 tag 时避免重名冲突（可重复执行）
    curl -s -H "Authorization: token $token" "$api/repos/$repo/releases/$release_id/assets" |
        python3 -c "
import json,sys
for a in json.load(sys.stdin):
    if a['name'] == '$name':
        print(a['id'])
" | while read -r aid; do
        [ -n "$aid" ] && curl -s -X DELETE -H "Authorization: token $token" \
            "$api/repos/$repo/releases/$release_id/assets/$aid" >/dev/null
    done
    curl -s -X POST -H "Authorization: token $token" \
        -F "attachment=@$f" "$api/repos/$repo/releases/$release_id/assets?name=$name" >/dev/null
    echo "已上传 $name → Release $tag"
done
