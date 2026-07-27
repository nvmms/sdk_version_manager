.pragma library

// SVM 内置 Provider 目录。
// key 是配置文件、CLI 和安装记录使用的稳定 ID，不应随显示名称变化。
// enabled=false 表示已经列入产品路线，但 Provider 尚未实现。
var sections = [
    {
        key: "download",
        name: qsTr("下载"),
        title: qsTr("下载中心"),
        description: qsTr("下载 SDK、Web 服务、数据库和缓存"),
        listTitle: qsTr("所有可用组件"),
        searchHint: qsTr("搜索 SDK、Web 或数据库")
    },
    {
        key: "web",
        name: qsTr("Web"),
        title: qsTr("Web 服务"),
        description: qsTr("下载并管理本地 Web 服务器"),
        listTitle: qsTr("Web 服务"),
        searchHint: qsTr("搜索 Web 服务")
    },
    {
        key: "database",
        name: qsTr("数据库"),
        title: qsTr("数据库"),
        description: qsTr("下载并管理数据库与数据服务"),
        listTitle: qsTr("数据库与缓存"),
        searchHint: qsTr("搜索数据库")
    }
]

var providers = [
    provider("flutter", "sdk", "Flutter", "跨平台应用开发", "F", "#55c2ff", true, [
        version("3.32.5", "stable", "1.1 GB", true),
        version("3.32.4", "stable", "1.1 GB"),
        version("3.33.0-1.0.pre", "beta", "1.2 GB")
    ]),
    provider("dart", "sdk", "Dart", "Dart SDK", "D", "#38c9bb", true, [
        version("3.8.1", "stable", "214 MB", true),
        version("3.7.3", "stable", "207 MB")
    ]),
    provider("java", "sdk", "Java", "JDK 与 JVM 工具链", "J", "#ff8b62", true, [
        version("24.0.2", "stable", "210 MB", true),
        version("21.0.8", "LTS", "198 MB"),
        version("17.0.16", "LTS", "187 MB"),
        version("11.0.28", "LTS", "181 MB")
    ]),
    provider("kotlin", "sdk", "Kotlin", "Kotlin 编译器", "K", "#a98bff"),
    provider("scala", "sdk", "Scala", "Scala 工具链", "Sc", "#ef6a67"),
    provider("groovy", "sdk", "Groovy", "JVM 动态语言", "Gr", "#69b9d1"),
    provider("python", "sdk", "Python", "Python 运行时与工具", "Py", "#ffd166", true, [
        version("3.13.5", "stable", "29 MB", true),
        version("3.12.11", "stable", "28 MB"),
        version("3.11.13", "maintenance", "27 MB"),
        version("3.14.0b4", "beta", "31 MB")
    ]),
    provider("node", "sdk", "Node.js", "JavaScript 运行时", "N", "#78d67a", true),
    provider("deno", "sdk", "Deno", "安全的 JavaScript 运行时", "De", "#d4d8de"),
    provider("bun", "sdk", "Bun", "JavaScript 运行时与工具包", "B", "#f2d6bd"),
    provider("php", "sdk", "PHP", "PHP 解释器与扩展", "P", "#9aa4ff", true, [
        version("8.4.10", "stable", "31 MB", true),
        version("8.3.23", "stable", "30 MB"),
        version("8.2.29", "security", "29 MB")
    ]),
    provider("ruby", "sdk", "Ruby", "Ruby 运行时", "Rb", "#e85d68"),
    provider("go", "sdk", "Go", "Go 工具链", "Go", "#55cde0"),
    provider("rust", "sdk", "Rust", "Rust 工具链", "Rs", "#e2a46f"),
    provider("dotnet", "sdk", ".NET", ".NET SDK 与运行时", ".N", "#9a79e8"),
    provider("swift", "sdk", "Swift", "Swift 工具链", "Sw", "#f58b62"),
    provider("lua", "sdk", "Lua", "Lua 解释器", "Lu", "#6e88dc"),
    provider("perl", "sdk", "Perl", "Perl 解释器", "Pe", "#79a9c9"),
    provider("elixir", "sdk", "Elixir", "Elixir 与 Mix", "Ex", "#a779c5"),
    provider("erlang", "sdk", "Erlang", "Erlang/OTP", "Er", "#d45a91"),
    provider("julia", "sdk", "Julia", "科学计算语言", "Ju", "#8ec76f"),
    provider("r", "sdk", "R", "统计计算环境", "R", "#6f9dd1"),

    provider("nginx", "web", "Nginx", "Web 与反向代理服务器", "N", "#55c986", true, [
        version("1.28.0", "stable", "4 MB", true),
        version("1.27.5", "mainline", "4 MB")
    ]),
    provider("apache-httpd", "web", "Apache HTTP Server", "经典 Web 服务器", "A", "#ef6e61", true, [
        version("2.4.63", "stable", "12 MB", true),
        version("2.4.62", "stable", "12 MB")
    ]),
    provider("caddy", "web", "Caddy", "自动 HTTPS Web 服务器", "C", "#72b9ff"),
    provider("tomcat", "web", "Apache Tomcat", "Java Servlet 容器", "T", "#e4bf63"),
    provider("traefik", "web", "Traefik", "云原生反向代理", "Tr", "#50b8e7"),

    provider("mysql", "database", "MySQL", "关系型数据库", "My", "#5aa7c6", true, [
        version("9.3.0", "innovation", "245 MB", true),
        version("8.4.5", "LTS", "238 MB"),
        version("8.0.42", "LTS", "231 MB")
    ]),
    provider("postgresql", "database", "PostgreSQL", "对象关系型数据库", "Pg", "#6f9bd1", true, [
        version("17.5", "stable", "310 MB", true),
        version("16.9", "stable", "302 MB"),
        version("15.13", "stable", "294 MB")
    ]),
    provider("mongodb", "database", "MongoDB", "文档数据库", "M", "#65c878", true, [
        version("8.0.10", "stable", "520 MB", true),
        version("7.0.21", "stable", "498 MB")
    ]),
    provider("redis", "database", "Redis", "内存数据存储", "R", "#ec665e", true, [
        version("8.0.2", "stable", "8 MB", true),
        version("7.4.4", "stable", "7 MB")
    ]),
    provider("mariadb", "database", "MariaDB", "MySQL 兼容数据库", "Ma", "#c89a72"),
    provider("sqlite", "database", "SQLite", "嵌入式关系型数据库", "Sq", "#63a9d4"),
    provider("clickhouse", "database", "ClickHouse", "列式分析数据库", "Ch", "#f4cf58"),
    provider("couchdb", "database", "CouchDB", "文档数据库", "Co", "#dc6a72"),
    provider("memcached", "database", "Memcached", "分布式内存缓存", "Me", "#8aa2bd")
]

function provider(key, section, name, summary, mark, color, enabled, versions) {
    return {
        key: key,
        section: section,
        name: name,
        summary: summary,
        mark: mark,
        color: color,
        enabled: enabled === true,
        // 版本必须来自 Provider 官方源或本地缓存，目录中不保存演示版本。
        versions: []
    }
}

function version(number, channel, size, recommended) {
    return {
        version: number,
        channel: channel,
        released: "—",
        size: size,
        recommended: recommended === true
    }
}

function section(key) {
    for (var i = 0; i < sections.length; ++i) {
        if (sections[i].key === key)
            return sections[i]
    }
    return sections[0]
}

function providersFor(sectionKey) {
    var result = []
    for (var i = 0; i < providers.length; ++i) {
        if (sectionKey === "download" || providers[i].section === sectionKey)
            result.push(providers[i])
    }
    return result
}
