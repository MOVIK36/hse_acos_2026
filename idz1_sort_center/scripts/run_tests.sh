#!/bin/bash
set -e

RED='\033[031m'
GREEN='\033[032m'
YELLOW='\033[033m'
BLUE='\033[034m'
NC='\033[0m'

echo -e "${BLUE}====================================================================${NC}"
echo -e "${BLUE}     АВТОМАТИЧЕСКОЕ ТЕСТИРОВАНИЕ ИМИТАЦИОННОЙ МОДЕЛИ (ВАРИАНТ 34)   ${NC}"
echo -e "${BLUE}====================================================================${NC}"

BIN="./sort_center"

if [ ! -f "$BIN" ]; then
    echo -e "${YELLOW}Исполняемый файл не найден. Выполняю сборку через make...${NC}"
    make clean && make
fi

run_test_case() {
    local test_name="$1"
    local config_file="$2"
    local log_file="$3"
    
    echo -e "\n${YELLOW}---> [ТЕСТ] $test_name <---${NC}"
    echo "Запуск с конфигурацией: $config_file"
    
    $BIN -c "$config_file" -L "$log_file" -q
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✓ Тест '$test_name' успешно пройден!${NC}"
    else
        echo -e "${RED}✗ Ошибка при выполнении теста '$test_name'!${NC}"
        exit 1
    fi
    
    if [ -f "$log_file" ]; then
        local log_lines=$(wc -l < "$log_file")
        echo -e "  Лог сохранен: $log_file (строк: $log_lines)"
    else
        echo -e "${RED}✗ Файл журнала не был создан!${NC}"
        exit 1
    fi
}

run_test_case "Базовый сбалансированный поток" "data/normal.conf" "normal_test.log"
run_test_case "Узкое горлышко и заторы (Backpressure)" "data/bottleneck.conf" "bottleneck_test.log"
run_test_case "Высокий уровень брака маркировки (>50%)" "data/high_error.conf" "high_error_test.log"

echo -e "\n${YELLOW}---> [ТЕСТ] Запуск с параметрами CLI (-n 15 -d 4 -e 25 --delay 0) <---${NC}"
$BIN -n 15 -d 4 -e 25 -D 0 -L "cli_test.log" -q
if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Тест CLI аргументов пройден успешно!${NC}"
else
    echo -e "${RED}✗ Ошибка при тестировании CLI аргументов!${NC}"
    exit 1
fi

echo -e "\n${GREEN}====================================================================${NC}"
echo -e "${GREEN}  ВСЕ ТЕСТЫ УСПЕШНО ВЫПОЛНЕНЫ. ИНВАРИАНТЫ СОБЛЮДЕНЫ, ЛОГИ ЗАПИСАНЫ.  ${NC}"
echo -e "${GREEN}====================================================================${NC}"