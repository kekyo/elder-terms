import { setTimeout as delay } from 'node:timers/promises';
import { describe, expect, it } from 'vitest';
import { waitForResult, toPass } from 'gestament/testing';
import {
  assertTerminalTextGridMatches,
  constrainedFontZoomSteps,
  defaultColumns,
  defaultRows,
  expectFixtureVteGridSize,
  expectWindowCellSize,
  moveMouseToTerminalCenter,
  rapidFontZoomBurstSteps,
  readTerminalGridLayout,
  readWindowCellLayout,
  runGtkTest,
  saveTerminalGridLayoutEvidence,
  scrollWheelBurstWithControl,
  scrollWheelWithControl,
  terminalTextGrid80x24FontScale11Path,
  terminalTextGrid80x24Path,
  terminalTextGrid81x25Path,
} from './gtk-test-helpers';

describe.concurrent('elder-terms-vte terminal layout', () => {
  it('fills the whole terminal image up to the right and bottom edges', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const layout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });
      await saveTerminalGridLayoutEvidence(evidence, layout, 'initial-layout');
      await assertTerminalTextGridMatches(
        layout.terminal,
        'fixture-terminal-initial',
        terminalTextGrid80x24Path,
        evidence
      );
    });
  });

  it('resizes to the requested whole-cell window size', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });
      await initialLayout.mainWindow.resizeTo(
        initialLayout.mainBounds.width + initialLayout.hints.widthIncrement,
        initialLayout.mainBounds.height + initialLayout.hints.heightIncrement
      );

      const resizedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expectWindowCellSize(layout, defaultColumns + 1, defaultRows + 1);
        await expectFixtureVteGridSize(
          app,
          defaultColumns + 1,
          defaultRows + 1
        );
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        resizedLayout,
        'whole-cell-resize-layout'
      );
      await assertTerminalTextGridMatches(
        resizedLayout.terminal,
        'fixture-terminal-whole-cell-resize',
        terminalTextGrid81x25Path,
        evidence
      );
    });
  });

  it('lets repeated border-like resize steps accumulate before snapping to cells', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });

      for (
        let index = 0;
        index <= initialLayout.hints.widthIncrement;
        ++index
      ) {
        const bounds = await initialLayout.mainWindow.bounds();
        await initialLayout.mainWindow.resizeTo(
          bounds.width + 1,
          bounds.height
        );
        await delay(10);
      }

      const resizedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expectWindowCellSize(layout, defaultColumns + 1, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns + 1, defaultRows);
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        resizedLayout,
        'incremental-border-resize-layout'
      );
    });
  });

  it('keeps the resized VTE grid size after Ctrl+wheel font zoom', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });

      const resizedColumns = defaultColumns + 1;
      const resizedRows = defaultRows + 1;
      await initialLayout.mainWindow.resizeTo(
        initialLayout.mainBounds.width + initialLayout.hints.widthIncrement,
        initialLayout.mainBounds.height + initialLayout.hints.heightIncrement
      );

      const resizedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expectWindowCellSize(layout, resizedColumns, resizedRows);
        await expectFixtureVteGridSize(app, resizedColumns, resizedRows);
        return layout;
      });

      await moveMouseToTerminalCenter(app, resizedLayout);
      await scrollWheelWithControl(app, -1);

      const zoomedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.hints.widthIncrement).not.toBe(
          resizedLayout.hints.widthIncrement
        );
        expect(layout.hints.heightIncrement).not.toBe(
          resizedLayout.hints.heightIncrement
        );
        expectWindowCellSize(layout, resizedColumns, resizedRows);
        await expectFixtureVteGridSize(app, resizedColumns, resizedRows);
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        zoomedLayout,
        'resized-font-zoom-layout'
      );
    });
  });

  it('rounds a fractional-cell window resize request to a whole-cell size', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      expect(initialLayout.hints.widthIncrement).toBeGreaterThan(1);
      expect(initialLayout.hints.heightIncrement).toBeGreaterThan(1);
      await initialLayout.mainWindow.resizeTo(
        initialLayout.mainBounds.width + initialLayout.hints.widthIncrement - 1,
        initialLayout.mainBounds.height +
          initialLayout.hints.heightIncrement -
          1
      );

      const resizedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.mainBounds.width).not.toBe(
          initialLayout.mainBounds.width +
            initialLayout.hints.widthIncrement -
            1
        );
        expect(layout.mainBounds.height).not.toBe(
          initialLayout.mainBounds.height +
            initialLayout.hints.heightIncrement -
            1
        );
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        resizedLayout,
        'fractional-cell-resize-layout'
      );
      await assertTerminalTextGridMatches(
        resizedLayout.terminal,
        'fixture-terminal-fractional-cell-resize',
        terminalTextGrid80x24Path,
        evidence
      );
    });
  });

  it('keeps the window constrained to whole VTE cells after Ctrl+wheel font zoom', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });

      await moveMouseToTerminalCenter(app, initialLayout);
      await scrollWheelWithControl(app, -1);

      const zoomedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.hints.widthIncrement).not.toBe(
          initialLayout.hints.widthIncrement
        );
        expect(layout.hints.heightIncrement).not.toBe(
          initialLayout.hints.heightIncrement
        );
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        zoomedLayout,
        'font-zoom-layout'
      );
      await assertTerminalTextGridMatches(
        zoomedLayout.terminal,
        'fixture-terminal-font-zoom',
        terminalTextGrid80x24FontScale11Path,
        evidence
      );
    });
  });

  it('lets repeated border-like resize steps accumulate after Ctrl+wheel font zoom', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });

      await moveMouseToTerminalCenter(app, initialLayout);
      await scrollWheelWithControl(app, -1);

      const zoomedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.hints.widthIncrement).not.toBe(
          initialLayout.hints.widthIncrement
        );
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
        return layout;
      });
      await delay(250);

      for (let index = 0; index <= zoomedLayout.hints.widthIncrement; ++index) {
        const bounds = await zoomedLayout.mainWindow.bounds();
        await zoomedLayout.mainWindow.resizeTo(bounds.width + 1, bounds.height);
        await delay(10);
      }

      const resizedLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expectWindowCellSize(layout, defaultColumns + 1, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns + 1, defaultRows);
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        resizedLayout,
        'zoomed-incremental-border-resize-layout'
      );
    });
  });

  it('keeps the VTE grid size during repeated Ctrl+wheel font zoom steps', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });

      await moveMouseToTerminalCenter(app, initialLayout);
      let currentLayout = initialLayout;
      for (const ySteps of [-1, -1, -1, 1, 1, 1]) {
        await scrollWheelWithControl(app, ySteps);
        await delay(25);
        currentLayout = await waitForResult(async () => {
          const layout = await readTerminalGridLayout(app);
          expectWindowCellSize(layout, defaultColumns, defaultRows);
          await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
          return layout;
        });
      }

      await saveTerminalGridLayoutEvidence(
        evidence,
        currentLayout,
        'repeated-font-zoom-steps-layout'
      );
    });
  });

  it('keeps terminal text stable after rapid Ctrl+wheel zoom in and out burst', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });

      await moveMouseToTerminalCenter(app, initialLayout);
      await scrollWheelBurstWithControl(app, [
        -rapidFontZoomBurstSteps,
        rapidFontZoomBurstSteps,
        -rapidFontZoomBurstSteps,
        rapidFontZoomBurstSteps,
      ]);
      await delay(100);

      const restoredLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.hints.widthIncrement).toBe(
          initialLayout.hints.widthIncrement
        );
        expect(layout.hints.heightIncrement).toBe(
          initialLayout.hints.heightIncrement
        );
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
        return layout;
      });
      await saveTerminalGridLayoutEvidence(
        evidence,
        restoredLayout,
        'rapid-font-zoom-burst-restored-layout'
      );
      await assertTerminalTextGridMatches(
        restoredLayout.terminal,
        'fixture-terminal-rapid-font-zoom-burst-restored',
        terminalTextGrid80x24Path,
        evidence
      );
    });
  });

  it('keeps the user resized VTE grid size after returning from font zoom', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const initialLayout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      expectWindowCellSize(initialLayout, defaultColumns, defaultRows);
      await toPass(async () => {
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });

      await moveMouseToTerminalCenter(app, initialLayout);
      await scrollWheelWithControl(app, -constrainedFontZoomSteps);

      const zoomedLayout = await waitForResult(async () => {
        const layout = await readWindowCellLayout(app);
        expect(layout.hints.widthIncrement).toBeGreaterThan(
          initialLayout.hints.widthIncrement
        );
        expect(layout.hints.heightIncrement).toBeGreaterThan(
          initialLayout.hints.heightIncrement
        );
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
        return layout;
      });
      await evidence.log('rapid font zoom in layout verified', {
        hints: zoomedLayout.hints,
        mainBounds: zoomedLayout.mainBounds,
      });
      await delay(250);

      await zoomedLayout.mainWindow.resizeTo(
        initialLayout.mainBounds.width,
        initialLayout.mainBounds.height
      );
      const constrainedLayout = await waitForResult(async () => {
        const layout = await readWindowCellLayout(app);
        expect(layout.hints.widthIncrement).toBe(
          zoomedLayout.hints.widthIncrement
        );
        expect(layout.hints.heightIncrement).toBe(
          zoomedLayout.hints.heightIncrement
        );
        return layout;
      });
      const resizedColumns =
        (constrainedLayout.mainBounds.width -
          constrainedLayout.hints.baseWidth) /
        constrainedLayout.hints.widthIncrement;
      const resizedRows =
        (constrainedLayout.mainBounds.height -
          constrainedLayout.hints.baseHeight) /
        constrainedLayout.hints.heightIncrement;
      expect(resizedColumns < defaultColumns || resizedRows < defaultRows).toBe(
        true
      );
      await toPass(async () => {
        await expectFixtureVteGridSize(app, resizedColumns, resizedRows);
      });
      await evidence.log('font zoom constrained layout verified', {
        hints: constrainedLayout.hints,
        mainBounds: constrainedLayout.mainBounds,
      });

      await scrollWheelWithControl(app, constrainedFontZoomSteps);

      const restoredLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expect(layout.hints.widthIncrement).toBe(
          initialLayout.hints.widthIncrement
        );
        expect(layout.hints.heightIncrement).toBe(
          initialLayout.hints.heightIncrement
        );
        expectWindowCellSize(layout, resizedColumns, resizedRows);
        await expectFixtureVteGridSize(app, resizedColumns, resizedRows);
        return layout;
      });
      await evidence.log('rapid font zoom restored layout verified', {
        hints: restoredLayout.hints,
        mainBounds: restoredLayout.mainBounds,
      });
    });
  });
});
