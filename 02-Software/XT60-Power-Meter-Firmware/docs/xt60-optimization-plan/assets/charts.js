/* XT60 功率计 · 优化分析报告图表脚本
 * 依赖：./_shared/js/echarts.min.js、./_shared/js/mermaid.min.js（均在本文件之前加载） */
(function () {
  'use strict';

  /* ---------- Mermaid 初始化（架构数据链路图） ---------- */
  if (window.mermaid) {
    mermaid.initialize({
      startOnLoad: true,
      theme: 'neutral',
      securityLevel: 'loose',
      fontFamily: "'Instrument Sans','Microsoft YaHei','PingFang SC',sans-serif"
    });
  }

  if (!window.echarts) {
    return;
  }

  /* ---------- 主题色（从 CSS 变量读取，保持与页面一致） ---------- */
  var style = getComputedStyle(document.documentElement);
  var accent = style.getPropertyValue('--accent').trim();
  var accent2 = style.getPropertyValue('--accent2').trim();
  var ink = style.getPropertyValue('--ink').trim();
  var muted = style.getPropertyValue('--muted').trim();
  var rule = style.getPropertyValue('--rule').trim();
  var bg2 = style.getPropertyValue('--bg2').trim();

  var baseText = {
    color: ink,
    fontFamily: "'Instrument Sans','Microsoft YaHei','PingFang SC',sans-serif",
    fontSize: 12
  };

  /* ---------- 图 1：能力成熟度雷达 ---------- */
  var radarEl = document.getElementById('chart-radar');
  if (radarEl) {
    var radar = echarts.init(radarEl, null, { renderer: 'svg' });
    radar.setOption({
      animation: false,
      color: [accent, accent2],
      textStyle: baseText,
      tooltip: { appendToBody: true },
      legend: {
        top: 0,
        textStyle: { color: ink },
        data: ['当前 v0.1.0', '目标 v1.0.0']
      },
      radar: {
        center: ['50%', '56%'],
        radius: '62%',
        indicator: [
          { name: '测量链路与精度', max: 5 },
          { name: '稳定性与容错', max: 5 },
          { name: '屏幕 UI 与交互', max: 5 },
          { name: '配置与个性化', max: 5 },
          { name: '数据持久化与导出', max: 5 },
          { name: '联网与远程能力', max: 5 },
          { name: '测试与质量基建', max: 5 },
          { name: '文档与可维护性', max: 5 }
        ],
        axisName: { color: muted, fontSize: 11.5 },
        splitLine: { lineStyle: { color: rule } },
        splitArea: { areaStyle: { color: [bg2, '#ffffff'] } },
        axisLine: { lineStyle: { color: rule } }
      },
      series: [
        {
          type: 'radar',
          symbolSize: 5,
          data: [
            {
              name: '当前 v0.1.0',
              value: [4, 4, 2.5, 2, 1.5, 0.5, 2, 4],
              areaStyle: { color: accent + '33' },
              lineStyle: { width: 2 }
            },
            {
              name: '目标 v1.0.0',
              value: [5, 5, 5, 4.5, 5, 4.5, 4.5, 4.5],
              areaStyle: { color: accent2 + '22' },
              lineStyle: { width: 2, type: 'dashed' }
            }
          ]
        }
      ]
    });
    window.addEventListener('resize', function () { radar.resize(); });
  }

  /* ---------- 图 3：新增功能价值-成本矩阵 ---------- */
  var matrixEl = document.getElementById('chart-matrix');
  if (matrixEl) {
    var matrix = echarts.init(matrixEl, null, { renderer: 'svg' });
    matrix.setOption({
      animation: false,
      textStyle: baseText,
      tooltip: {
        appendToBody: true,
        formatter: function (p) {
          return '<b>' + p.data.name + '</b><br/>开发成本：' + p.data.value[0] +
            ' 人日<br/>用户价值：' + p.data.value[1] + ' / 5';
        }
      },
      legend: {
        top: 0,
        textStyle: { color: ink },
        data: ['建议实施', '已否决']
      },
      grid: { left: 56, right: 40, top: 48, bottom: 46 },
      xAxis: {
        name: '开发成本（人日）',
        nameLocation: 'middle',
        nameGap: 30,
        min: 0,
        max: 6.5,
        splitLine: { lineStyle: { color: rule } },
        axisLine: { lineStyle: { color: rule } },
        axisLabel: { color: muted, formatter: '{value} d' }
      },
      yAxis: {
        name: '用户价值',
        nameGap: 18,
        min: 0.5,
        max: 5.5,
        splitLine: { lineStyle: { color: rule } },
        axisLine: { lineStyle: { color: rule } },
        axisLabel: { color: muted }
      },
      series: [
        {
          name: '建议实施',
          type: 'scatter',
          symbolSize: function (val) { return val[2]; },
          itemStyle: { color: accent, opacity: 0.85 },
          label: {
            show: true,
            position: 'top',
            color: ink,
            fontSize: 11,
            formatter: function (p) { return p.data.name; }
          },
          data: [
            { name: 'N1 Web 控制台', value: [5.5, 5, 44] },
            { name: 'N2 历史与导出', value: [4, 5, 40] },
            { name: 'N3 OTA 升级', value: [2.5, 4, 30] },
            { name: 'N4 告警保护', value: [1.5, 5, 38] },
            { name: 'N5 MQTT / HA', value: [3, 4, 30] },
            { name: 'N6 成本与统计', value: [1.5, 3, 24] },
            { name: 'N7 多语言', value: [2, 3, 26] }
          ]
        },
        {
          name: '已否决',
          type: 'scatter',
          symbolSize: function (val) { return val[2]; },
          itemStyle: { color: muted, opacity: 0.55 },
          label: {
            show: true,
            position: 'bottom',
            color: muted,
            fontSize: 11,
            formatter: function (p) { return p.data.name; }
          },
          data: [
            { name: 'X1 USB PD 协议解码', value: [5.8, 1.5, 20] },
            { name: 'X2 蓝牙 App', value: [4.6, 1.2, 20] }
          ]
        }
      ]
    });
    window.addEventListener('resize', function () { matrix.resize(); });
  }

  /* ---------- 图 4：三阶段投入分布 ---------- */
  var phasesEl = document.getElementById('chart-phases');
  if (phasesEl) {
    var phases = echarts.init(phasesEl, null, { renderer: 'svg' });
    phases.setOption({
      animation: false,
      color: [accent, accent2, muted],
      textStyle: baseText,
      tooltip: {
        appendToBody: true,
        trigger: 'axis',
        axisPointer: { type: 'shadow' },
        valueFormatter: function (v) { return v + ' 人日'; }
      },
      legend: { top: 0, textStyle: { color: ink } },
      grid: { left: 48, right: 24, top: 48, bottom: 40 },
      xAxis: {
        type: 'category',
        data: ['P0 · 质量与体验', 'P1 · 联网与数据', 'P2 · 差异化发布'],
        axisLine: { lineStyle: { color: rule } },
        axisLabel: { color: ink }
      },
      yAxis: {
        type: 'value',
        name: '人日',
        splitLine: { lineStyle: { color: rule } },
        axisLabel: { color: muted }
      },
      series: [
        {
          name: '固件功能实现',
          type: 'bar',
          stack: 'total',
          barWidth: 46,
          label: { show: true, color: '#ffffff', formatter: '{c}' },
          data: [6.0, 12.5, 8.0]
        },
        {
          name: '测试与验证',
          type: 'bar',
          stack: 'total',
          label: { show: true, color: '#ffffff', formatter: '{c}' },
          data: [2.5, 3.5, 2.5]
        },
        {
          name: '文档与发布',
          type: 'bar',
          stack: 'total',
          label: { show: true, color: '#ffffff', formatter: '{c}' },
          data: [1.5, 2.0, 3.5]
        }
      ]
    });
    window.addEventListener('resize', function () { phases.resize(); });
  }
})();