#!/bin/bash

# 使用环境变量 MAKE 指定构建命令；若未设置，则默认使用 make。
# 这样可以在不同环境中灵活切换为 gmake、make -j 等自定义命令。
make="${MAKE:-make}"

# 终端输出颜色与样式定义，用于让评分日志更易读。
# RED/BLUE/GREEN/ORANGE 分别表示红/蓝/绿/橙色，BOLD 表示加粗，NONE 用于重置样式。
RED='\033[0;31m'
BLUE='\033[0;34m'
GREEN='\033[0;32m'
ORANGE='\033[0;33m'
BOLD='\033[1m'
NONE='\033[0m'

# Lab 3 一共包含 5 个评分子任务。
# TIME 仅用于展示预计耗时，这里按每个子任务约 10 秒估算。
LAB3_PART_NUM=5
TIME=$(($LAB3_PART_NUM * 10))

# Path
# 路径相关变量：
# - grade_dir：当前评分脚本所在目录
# - expect_dir：Expect 脚本目录，每个子任务对应一个 .exp 自动化测试脚本
# - root_dir：项目根目录
# - scripts_dir：项目中的 scripts 目录
# - config_dir：各子任务对应的构建配置文件目录
grade_dir=$(dirname $(readlink -f "$0"))
expect_dir=$grade_dir/expects
root_dir=$grade_dir/../..
scripts_dir=$root_dir/scripts
config_dir=$scripts_dir/build

# 打印评分开始提示与预计耗时。
echo -e "${BOLD}===============${NONE}"
echo -e "${BLUE}Grading lab 3...(may take ${TIME} seconds)${NONE}"

# score 用于累计所有子任务得分。
# 切换到项目根目录后统一执行构建与测试，避免相对路径受当前目录影响。
# 先执行一次 distclean，确保评分从干净环境开始，减少增量构建带来的干扰。
# 标准输出重定向到 .build_log，便于在需要时查看构建过程。
score=0
cd $root_dir
$make distclean >.build_log

# 依次执行 lab3-1 到 lab3-5 的评分流程：
# 1. 将对应子任务的配置文件复制为项目根目录下的 .config
# 2. 根据该配置重新编译整个项目
# 3. 运行对应的 Expect 自动测试脚本
# 4. 将测试脚本返回值（该部分得分）累加到总分中
#
# 其中：
# - 构建标准输出写入 .build_log
# - 构建标准错误写入 .build_stderr
# 这样在测试失败时可以进一步排查编译问题。
for i in $(seq 1 $LAB3_PART_NUM); do
    cp $config_dir/lab3-$i.config $root_dir/.config
    $make build >.build_log 2>.build_stderr
    $expect_dir/lab3-$i.exp
    score=$(($score + $?))
done

# 输出最终总分。满分为 100 分。
echo -e "${BOLD}===============${NONE}"
echo -e "${GREEN}Score: $score/100${NONE}"

# 清理评分过程中生成的临时构建日志文件，保持工作目录整洁。
rm .build_log
rm .build_stderr
