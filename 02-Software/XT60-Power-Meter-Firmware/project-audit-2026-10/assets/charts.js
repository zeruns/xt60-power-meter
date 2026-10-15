// assets/charts.js —— XT60 功率计固件项目审计报告图表
// 全部图表配色取自 CSS 变量，保证与报告主题一致。
(function () {
  'use strict';

  var style = getComputedStyle(document.documentElement);
  var accent = style.getPropertyValue('--accent').trim();
  var accent2 = style.getPropertyValue('--accent2').trim();
  var ink = style.getPropertyValue('--ink').trim();
  var muted = style.getPropertyValue('--muted').trim();
  var rule = style.getPropertyValue('--rule').trim();
  var bg2 = style.getPropertyValue('--bg2').trim();
  var p0 = style.getPropertyValue('--p0').trim();
  var p1 = style.getPropertyValue('--p1').trim();
  var p2 = style.getPropertyValue('--p2').trim();
  var ok = style.getPropertyValue('--ok').trim();

  var baseTooltip = { appendToBody: true, backgroundColor: bg2, borderColor: rule, textStyle: { color: ink } };

  // ---------- 图 1 · 功能完整性状态分布（环形图） ----------
  var c1 = echarts.init(document.getElementById('chart-completeness'), null, { renderer: 'svg' });
  c1.setOption({
    animation: false,
    tooltip: Object.assign({}, baseTooltip, {
      trigger: 'item',
      formatter: '{b}：{c} 项（{d}%）'
    }),
    legend: { bottom: 0, textStyle: { color: muted } },
    series: [{
      type: 'pie',
      radius: ['42%', '68%'],
      center: ['50%', '45%'],
      label: { color: ink, formatter: '{b}\n{c} 项' },
      labelLine: { lineStyle: { color: rule } },
      data: [
        { name: '完整（宣称与实现一致）', value: 5, itemStyle: { color: ok } },
        { name: '部分（有实现但不完整）', value: 4, itemStyle: { color: accent2 } },
        { name: '仅宣称（空桩/无代码）', value: 2, itemStyle: { color: p0 } }
      ]
    }]
  });
  window.addEventListener('resize', function () { c1.resize(); });

  // ---------- 图 2 · 问题严重度分布（环形图） ----------
  var c2 = echarts.init(document.getElementById('chart-severity'), null, { renderer: 'svg' });
  c2.setOption({
    animation: false,
    tooltip: Object.assign({}, baseTooltip, { trigger: 'item', formatter: '{b}：{c} 项（{d}%）' }),
    legend: { bottom: 0, textStyle: { color: muted } },
    series: [{
      type: 'pie',
      radius: ['42%', '68%'],
      center: ['50%', '45%'],
      label: { color: ink, formatter: '{b}\n{c} 项' },
      labelLine: { lineStyle: { color: rule } },
      data: [
        { name: 'P0 必须修', value: 4, itemStyle: { color: p0 } },
        { name: 'P1 应该修', value: 17, itemStyle: { color: p1 } },
        { name: 'P2 建议修', value: 21, itemStyle: { color: p2 } }
      ]
    }]
  });
  window.addEventListener('resize', function () { c2.resize(); });

  // ---------- 图 3 · 各层问题分布（堆叠柱状图） ----------
  var c3 = echarts.init(document.getElementById('chart-layers'), null, { renderer: 'svg' });
  c3.setOption({
    animation: false,
    tooltip: Object.assign({}, baseTooltip, { trigger: 'axis', axisPointer: { type: 'shadow' } }),
    legend: { bottom: 0, textStyle: { color: muted } },
    grid: { left: 40, right: 16, top: 30, bottom: 56 },
    xAxis: {
      type: 'category',
      data: ['驱动层', '服务/中间件', 'UI 层', '工程化/开源'],
      axisLine: { lineStyle: { color: rule } },
      axisLabel: { color: muted, interval: 0, rotate: 18, fontSize: 11 }
    },
    yAxis: {
      type: 'value', name: '问题数',
      nameTextStyle: { color: muted },
      axisLine: { lineStyle: { color: rule } },
      axisLabel: { color: muted },
      splitLine: { lineStyle: { color: rule } }
    },
    series: [
      { name: 'P0', type: 'bar', stack: 'total', itemStyle: { color: p0 }, data: [0, 0, 2, 2] },
      { name: 'P1', type: 'bar', stack: 'total', itemStyle: { color: p1 }, data: [3, 4, 3, 7] },
      { name: 'P2', type: 'bar', stack: 'total', itemStyle: { color: p2 }, data: [5, 6, 5, 5] }
    ]
  });
  window.addEventListener('resize', function () { c3.resize(); });

  // ---------- 图 4 · 功能路线图：价值 × 复杂度（散点/气泡） ----------
  // 数据结构：[复杂度(1=小 2=中 3=大), 价值(1~5), 优先级, 名称]
  var roadmap = [
    [1, 5.0, 'P0', 'LICENSE 定稿'],
    [1, 4.8, 'P0', '过流/过压告警'],
    [1, 4.6, 'P0', '峰值/浪涌统计'],
    [1, 4.2, 'P0', '累计 Ah'],
    [2, 5.0, 'P0', 'CSV/串口导出'],
    [2, 4.3, 'P1', '阈值触发捕获'],
    [2, 3.2, 'P1', '校准导入导出'],
    [2, 3.0, 'P1', '温补/预热提示'],
    [1, 3.3, 'P1', '120s 窗修复'],
    [1, 3.1, 'P1', '统计面板'],
    [1, 2.8, 'P1', '息屏/锁屏/单位'],
    [2, 3.8, 'P2', 'OTA 升级闭环'],
    [3, 4.4, 'P2', 'WiFi + Web 控制台'],
    [3, 4.0, 'P2', '连续记录到 Flash'],
    [3, 3.6, 'P2', '上位机软件']
  ];
  var prioColor = { P0: p0, P1: p1, P2: p2 };
  var prioSize = { P0: 26, P1: 19, P2: 15 };

  var c4 = echarts.init(document.getElementById('chart-roadmap'), null, { renderer: 'svg' });
  c4.setOption({
    animation: false,
    tooltip: Object.assign({}, baseTooltip, {
      trigger: 'item',
      formatter: function (p) {
        return p.data[3] + '<br/>复杂度：' + ['小', '中', '大'][p.data[0] - 1] +
               ' · 优先级：' + p.data[2];
      }
    }),
    grid: { left: 56, right: 30, top: 36, bottom: 52 },
    xAxis: {
      type: 'value', name: '复杂度 →', nameLocation: 'middle', nameGap: 30,
      min: 0.5, max: 3.5,
      axisLine: { lineStyle: { color: rule } },
      axisLabel: {
        color: muted,
        formatter: function (v) { return { 1: '小', 2: '中', 3: '大' }[v] || ''; }
      },
      splitLine: { lineStyle: { color: rule } }
    },
    yAxis: {
      type: 'value', name: '产品价值 →', min: 2.2, max: 5.5,
      nameTextStyle: { color: muted },
      axisLine: { lineStyle: { color: rule } },
      axisLabel: { color: muted },
      splitLine: { lineStyle: { color: rule } }
    },
    series: [{
      type: 'scatter',
      symbolSize: function (d) { return prioSize[d[2]]; },
      itemStyle: {
        color: function (p) { return prioColor[p.data[2]]; },
        opacity: 0.82
      },
      label: {
        show: true,
        position: 'right',
        color: ink,
        fontSize: 11,
        formatter: function (p) { return p.data[3]; }
      },
      data: roadmap
    }]
  });
  window.addEventListener('resize', function () { c4.resize(); });

  // ---------- 甘特图（Mermaid） ----------
  // 显式 run()：仅靠 startOnLoad 在脚本尾部加载时可能错过渲染时机，导致空白
  if (typeof mermaid !== 'undefined') {
    mermaid.initialize({ startOnLoad: false, theme: 'neutral', securityLevel: 'loose' });
    mermaid.run({ querySelector: '.mermaid' }).catch(function (e) {
      console.error('[mermaid] 渲染失败:', e);
    });
  }
})();
