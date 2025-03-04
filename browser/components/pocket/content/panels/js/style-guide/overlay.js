import React from "react";
import ReactDOM from "react-dom";
import Header from "../components/Header/Header";
import ArticleList from "../components/ArticleList/ArticleList";
import Button from "../components/Button/Button";
import PopularTopics from "../components/PopularTopics/PopularTopics";
import TagPicker from "../components/TagPicker/TagPicker";

var StyleGuideOverlay = function(options) {};

StyleGuideOverlay.prototype = {
  create() {
    // TODO: Wrap popular topics component in JSX to work without needing an explicit container hierarchy for styling
    ReactDOM.render(
      <div>
        <h3>JSX Components:</h3>
        <h4 className="stp_styleguide_h4">Button</h4>
        <Button style="text" url="https://example.org">
          Text Button
        </Button>
        <br />
        <Button style="primary">Primary Button</Button>
        <br />
        <Button style="secondary">Secondary Button</Button>
        <span className="stp_button_wide">
          <Button style="primary">Primary Wide Button</Button>
        </span>
        <span className="stp_button_wide">
          <Button style="secondary">Secondary Wide Button</Button>
        </span>
        <h4 className="stp_styleguide_h4">Header</h4>
        <Header>
          <Button style="primary">View My List</Button>
        </Header>
        <h4 className="stp_styleguide_h4">PopularTopics</h4>
        <PopularTopics
          pockethost={`getpocket.com`}
          utmsource={`styleguide`}
          topics={[
            { title: "Self Improvement", topic: "self-improvement" },
            { title: "Food", topic: "food" },
            { title: "Entertainment", topic: "entertainment" },
            { title: "Science", topic: "science" },
          ]}
        />
        <h4 className="stp_styleguide_h4">ArticleList</h4>
        <ArticleList
          articles={[
            {
              title: "Article Title",
              publisher: "Publisher",
              thumbnail:
                "https://img-getpocket.cdn.mozilla.net/80x80/https://www.raritanheadwaters.org/wp-content/uploads/2020/04/red-fox.jpg",
              url: "https://example.org",
              alt: "Alt Text",
            },
            {
              title: "Article Title",
              publisher: "Publisher",
              thumbnail:
                "https://img-getpocket.cdn.mozilla.net/80x80/https://www.raritanheadwaters.org/wp-content/uploads/2020/04/red-fox.jpg",
              url: "https://example.org",
              alt: "Alt Text",
            },
            {
              title: "Article Title",
              publisher: "Publisher",
              thumbnail:
                "https://img-getpocket.cdn.mozilla.net/80x80/https://www.raritanheadwaters.org/wp-content/uploads/2020/04/red-fox.jpg",
              url: "https://example.org",
              alt: "Alt Text",
            },
          ]}
        />
        <h4 className="stp_styleguide_h4">TagPicker</h4>
        <TagPicker tags={[`futurism`, `politics`, `mozilla`]} />
        <h3>Typography:</h3>
        <h2 className="header_large">.header_large</h2>
        <h3 className="header_medium">.header_medium</h3>
        <p>paragraph</p>
        <h3>Native Elements:</h3>
        <h4 className="stp_styleguide_h4">Horizontal Rule</h4>
        <hr />
      </div>,
      document.querySelector(`#stp_style_guide_components`)
    );
  },
};

export default StyleGuideOverlay;
