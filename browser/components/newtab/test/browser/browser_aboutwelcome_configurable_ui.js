"use strict";

const { ExperimentFakes } = ChromeUtils.import(
  "resource://testing-common/NimbusTestUtils.jsm"
);

const BASE_SCREEN_CONTENT = {
  title: "Step 1",
  primary_button: {
    label: "Next",
    action: {
      navigate: true,
    },
  },
  secondary_button: {
    label: "link",
  },
};

const makeTestContent = (id, contentAdditions, order = 0) => {
  return {
    id,
    order,
    content: Object.assign({}, BASE_SCREEN_CONTENT, contentAdditions),
  };
};

async function openAboutWelcome(json) {
  if (json) {
    await setAboutWelcomeMultiStage(json);
  }

  let tab = await BrowserTestUtils.openNewForegroundTab(
    gBrowser,
    "about:welcome",
    true
  );
  registerCleanupFunction(() => {
    BrowserTestUtils.removeTab(tab);
  });
  return tab.linkedBrowser;
}

async function test_computed_styles(
  browser,
  elementSelector,
  expectedStyles = {},
  unexpectedStyles = {}
) {
  await ContentTask.spawn(
    browser,
    [elementSelector, expectedStyles, unexpectedStyles],
    async ([selector, expected, unexpected]) => {
      const element = await ContentTaskUtils.waitForCondition(() =>
        content.document.querySelector(selector)
      );
      const computedStyles = content.window.getComputedStyle(element);
      Object.entries(expected).forEach(([attr, val]) =>
        is(
          computedStyles[attr],
          val,
          `${selector} should have computed ${attr} of ${val}`
        )
      );
      Object.entries(unexpected).forEach(([attr, val]) =>
        isnot(
          computedStyles[attr],
          val,
          `${selector} should not have computed ${attr} of ${val}`
        )
      );
    }
  );
}

/**
 * Test rendering a screen in about welcome with decorative noodles
 */
add_task(async function test_aboutwelcome_with_noodles() {
  const TEST_NOODLE_CONTENT = makeTestContent("TEST_NOODLE_STEP", {
    has_noodles: true,
  });
  const TEST_NOODLE_JSON = JSON.stringify([TEST_NOODLE_CONTENT]);
  let browser = await openAboutWelcome(TEST_NOODLE_JSON);

  await test_screen_content(
    browser,
    "renders screen with noodles",
    // Expected selectors:
    [
      "main.TEST_NOODLE_STEP[pos='center']",
      "div.noodle.purple-C",
      "div.noodle.orange-L",
      "div.noodle.outline-L",
      "div.noodle.yellow-circle",
    ]
  );
});

/**
 * Test rendering a screen with a customized logo
 */
add_task(async function test_aboutwelcome_with_customized_logo() {
  const TEST_LOGO_URL = "chrome://branding/content/icon64.png";
  const TEST_LOGO_CONTENT = makeTestContent("TEST_LOGO_STEP", {
    logo: {
      height: "50px",
      imageURL: TEST_LOGO_URL,
    },
  });
  const TEST_LOGO_JSON = JSON.stringify([TEST_LOGO_CONTENT]);
  const LOGO_HEIGHT = TEST_LOGO_CONTENT.content.logo.height;
  const EXPECTED_LOGO_STYLE = `background: rgba(0, 0, 0, 0) url("${TEST_LOGO_URL}") no-repeat scroll center center / contain; height: ${LOGO_HEIGHT}`;
  const DEFAULT_LOGO_STYLE =
    'background: rgba(0, 0, 0, 0) url("chrome://branding/content/about-logo.svg")';
  let browser = await openAboutWelcome(TEST_LOGO_JSON);

  await test_screen_content(
    browser,
    "renders screen with customized logo",
    // Expected selectors:
    [
      "main.TEST_LOGO_STEP[pos='center']",
      `div.brand-logo[style*='${EXPECTED_LOGO_STYLE}']`,
    ],
    // Unexpected selectors:
    [`div.brand-logo[style*='${DEFAULT_LOGO_STYLE}']`]
  );
});

/**
 * Test rendering a screen with a URL value and default color for backdrop
 */
add_task(async function test_aboutwelcome_with_url_backdrop() {
  const TEST_BACKDROP_URL = `url("chrome://activity-stream/content/data/content/assets/proton-bkg.avif")`;
  const TEST_BACKDROP_VALUE = `#212121 ${TEST_BACKDROP_URL} center/cover no-repeat fixed`;
  const TEST_URL_BACKDROP_CONTENT = makeTestContent("TEST_URL_BACKDROP_STEP");

  let doExperimentCleanup = await ExperimentFakes.enrollWithFeatureConfig({
    featureId: "aboutwelcome",
    enabled: true,
    value: {
      backdrop: TEST_BACKDROP_VALUE,
      screens: [TEST_URL_BACKDROP_CONTENT],
    },
  });
  let browser = await openAboutWelcome();

  await test_screen_content(
    browser,
    "renders screen with background image",
    // Expected selectors:
    [`div.outer-wrapper.onboardingContainer[style*='${TEST_BACKDROP_URL}']`]
  );
  await doExperimentCleanup();
});

/**
 * Test rendering a screen with a color name for backdrop
 */
add_task(async function test_aboutwelcome_with_color_backdrop() {
  const TEST_BACKDROP_COLOR = "transparent";
  const TEST_BACKDROP_COLOR_CONTENT = makeTestContent(
    "TEST_COLOR_NAME_BACKDROP_STEP"
  );

  let doExperimentCleanup = await ExperimentFakes.enrollWithFeatureConfig({
    featureId: "aboutwelcome",
    enabled: true,
    value: {
      backdrop: TEST_BACKDROP_COLOR,
      screens: [TEST_BACKDROP_COLOR_CONTENT],
    },
  });
  let browser = await openAboutWelcome();

  await test_screen_content(
    browser,
    "renders screen with background color",
    // Expected selectors:
    [`div.outer-wrapper.onboardingContainer[style*='${TEST_BACKDROP_COLOR}']`]
  );
  await doExperimentCleanup();
});

/**
 * Test rendering a screen with a title that's fancy, slim, and larger
 */
add_task(async function test_aboutwelcome_with_title_styles() {
  const TEST_TITLE_STYLE = "fancy slim larger";
  const TEST_TITLE_STYLE_CONTENT = makeTestContent("TEST_TITLE_STYLE_STEP", {
    title_style: TEST_TITLE_STYLE,
  });

  const TEST_TITLE_STYLE_JSON = JSON.stringify([TEST_TITLE_STYLE_CONTENT]);
  let browser = await openAboutWelcome(TEST_TITLE_STYLE_JSON);

  await test_screen_content(
    browser,
    "renders screen with customized title style",
    // Expected selectors:
    [`div.welcome-text.fancy.slim.larger`]
  );

  await test_computed_styles(
    browser,
    "#mainContentHeader",
    // Expected styles:
    {
      "font-weight": "276",
      "font-size": "36px",
      animation: "50s linear 0s infinite normal none running shine",
    },
    // Unexpected styles:
    {
      color: "rgb(21, 20, 26)",
    }
  );
});

/**
 * Test rendering a screen with an image for the dialog window's background
 */
add_task(async function test_aboutwelcome_with_background() {
  const BACKGROUND_URL =
    "chrome://activity-stream/content/data/content/assets/proton-bkg.avif";
  const TEST_BACKGROUND_CONTENT = makeTestContent("TEST_BACKGROUND_STEP", {
    background: `url(${BACKGROUND_URL}) no-repeat center/cover`,
  });

  const TEST_BACKGROUND_JSON = JSON.stringify([TEST_BACKGROUND_CONTENT]);
  let browser = await openAboutWelcome(TEST_BACKGROUND_JSON);

  await test_screen_content(
    browser,
    "renders screen with dialog background image",
    // Expected selectors:
    [`div.main-content[style*='${BACKGROUND_URL}'`]
  );
});

/**
 * Test rendering a screen with a text color override
 */
add_task(async function test_aboutwelcome_with_text_color_override() {
  await SpecialPowers.pushPrefEnv({
    set: [
      // Override the system color scheme to dark
      ["ui.systemUsesDarkTheme", 1],
    ],
  });

  let screens = [];
  for (let order = 0; order < 2; order++) {
    // we need at least two screens to test the step indicator
    screens.push(
      makeTestContent(
        "TEST_TEXT_COLOR_OVERRIDE_STEP",
        {
          text_color: "dark",
          background: "white",
        },
        order
      )
    );
  }

  let doExperimentCleanup = await ExperimentFakes.enrollWithFeatureConfig({
    featureId: "aboutwelcome",
    enabled: true,
    value: {
      screens,
    },
  });
  let browser = await openAboutWelcome(JSON.stringify(screens));

  await test_screen_content(
    browser,
    "renders screen with dark text",
    // Expected selectors:
    [`main.screen.dark-text`],
    // Unexpected selectors:
    [`main.screen.light-text`]
  );

  // Ensure title inherits light text color
  await test_computed_styles(
    browser,
    "#mainContentHeader",
    // Expected styles:
    {
      color: "rgb(21, 20, 26)",
    }
  );

  // Ensure next step indicator inherits light color
  await test_computed_styles(
    browser,
    ".indicator:not(.current)",
    // Expected styles:
    {
      color: "rgb(251, 251, 254)",
    }
  );

  await doExperimentCleanup();
});
