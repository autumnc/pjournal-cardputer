#pragma once
// 拼音分词补充词典：主词典缺失或需显式音节分界的词。
// 单引号编码分词时（如 xi'an、an'guang）与普通编码全码匹配时（xian、anguang）
// 都会查这张表。syllables 用空格分隔音节，word 为对应汉字。
struct SegEntry {
    const char *syllables;  // e.g. "an guang"
    const char *word;       // e.g. "暗光"
};

static const SegEntry SEG_TABLE[] = {
    { "an guang", "暗光" },      // 主词典缺失
    { "xi an", "西安" },          // 主词典仅存联想数据，无词组词条
    { "fang an", "方案" },
    { "dang an", "档案" },
    { "ti an", "提案" },
    { "di an", "堤岸" },
    { "ming an", "命案" },
    { "ping an", "平安" },
    { "xue an", "血案" },
    { "li an", "立案" },          // li'an 与 连/脸(lian) 冲突
    { "yi an", "议案" },
    { "yu an", "预案" },          // yu'an 与 元/愿(yuan) 冲突
    { "bi an", "彼岸" },          // bi'an 与 边/变(bian) 冲突
    { "yan an", "延安" },
    { "xin an", "心安" },
    { "xin ai", "心爱" },
    { "qin ai", "亲爱" },
    { "guan ai", "关爱" },
    { "pi ao", "皮袄" },
    { "chang e", "嫦娥" },
    { "ji e", "饥饿" },           // ji'e 与 借(jie) 冲突
    { "qi e", "企鹅" },           // qi'e 与 且/切(qie) 冲突
    { "ke e", "可恶" },
    { "ji ang", "激昂" },         // ji'ang 与 江(jiang) 冲突
    { "ku ai", "酷爱" },          // ku'ai 与 快(kuai) 冲突
    { "jiao ao", "骄傲" },
    { "tian an men", "天安门" },
    { "dang an guan", "档案馆" },
};

static const int SEG_TABLE_COUNT = (int)(sizeof(SEG_TABLE) / sizeof(SEG_TABLE[0]));
