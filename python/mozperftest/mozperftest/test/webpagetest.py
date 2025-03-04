from mozperftest.layers import Layer
import json
import requests
import time
from threading import Thread
import re
import mozperftest.utils as utils
import pathlib
from mozperftest.runner import HERE

WPT_METRICS = [
    "firstContentfulPaint",
    "timeToContentfulPaint",
    "visualComplete90",
    "firstPaint",
    "visualComplete99",
    "visualComplete",
    "SpeedIndex",
    "bytesIn",
    "bytesOut",
    "TTFB",
    "fullyLoadedCPUms",
    "fullyLoadedCPUpct",
    "domElements",
    "domContentLoadedEventStart",
    "domContentLoadedEventEnd",
    "loadEventStart",
    "loadEventEnd",
]
ACCEPTED_BROWSERS = ["Chrome", "Firefox"]

ACCEPTED_CONNECTIONS = [
    "DSL",
    "Cable",
    "FIOS",
    "Dial",
    "Edge",
    "2G",
    "3GSlow",
    "3GFast",
    "3G",
    "4G",
    "LTE",
    "Native",
    "custom",
]

ACCEPTED_STATISTICS = ["average", "median", "standardDeviation"]


class WPTTimeOutError(Exception):
    """
    This error is raised if a request that you have made has not returned results within a
    specified time, for this code that timeout is ~6 hours.
    """

    pass


class WPTBrowserSelectionError(Exception):
    """
    This error is raised if you provide an invalid browser option when requesting a test
    The only browsers allowed are specified the ACCEPTED_BROWSERS list at the top of the code
    browser must be a case-sensitive match in the list.
    """

    pass


class WPTLocationSelectionError(Exception):
    """
    This error is raised if you provide an invalid testing location option when requesting a test
    Acceptable locations are specified here: https://www.webpagetest.org/getTesters.php?f=html
    Connection type must be a case-sensitive match
    For example to test in Virginia, USA you would put ec2-us-east1 as your location.
    """

    pass


class WPTInvalidConnectionSelection(Exception):
    """
    This error is raised if you provide an invalid connection option when requesting a test
    The only connection allowed are specified the ACCEPTED_CONNECTIONS list at the top of the code
    Connection type must be a case-sensitive match in the list.
    """

    pass


class WPTDataProcessingError(Exception):
    """
    This error is raised when a value you were expecting in your webpagetest result is not there.
    """

    pass


class WPTInvalidURLError(Exception):
    """
    This error is raised if you provide an invalid website url when requesting a test
    A website must be in the format {domain_name}.{top_level_domain}
    for example "google.ca" and "youtube.com" both work and are valid website urls, but
    "google" and "youtube" are not.
    """

    pass


class WPTErrorWithWebsite(Exception):
    """
    This error is raised if the first and repeat view results of the test you requested
    is not in-line with what is returned. For instance if you request 3 runs with first
    and repeat view and results show 3 first view and 2 repeat view tests this exception
    is raised.
    """

    pass


class WPTInvalidStatisticsError(Exception):
    """
    This error is raised if the first and repeat view results of the test you requested
    is not in-line with what is returned. For instance if you request 3 runs with first
    and repeat view and results show 3 first view and 2 repeat view tests this exception
    is raised.
    """

    pass


class PropagatingErrorThread(Thread):
    def run(self):
        self.exc = None
        try:
            self._target(*self._args, **self._kwargs)
        except Exception as e:
            self.exc = e

    def join(self, timeout=None):
        super(PropagatingErrorThread, self).join()
        if self.exc:
            raise self.exc


class WebPageTestData:
    def open_data(self, data):
        return {
            "name": "webpagetest",
            "subtest": data["name"],
            "data": [
                {"file": "webpagetest", "value": value, "xaxis": xaxis}
                for xaxis, value in enumerate(data["values"])
            ],
        }

    def transform(self, data):
        return data

    merge = transform


class WebPageTest(Layer):
    """
    This is the webpagetest layer, it is responsible for sending requests to run a webpagetest
    pageload test, receiving the results as well processing them into a useful data format.
    """

    name = "webpagetest"
    activated = False
    arguments = {
        "no-video": {
            "action": "store_true",
            "default": False,
            "help": "Disable video, required for calculating Speed Index and filmstrip view",
        },
        "no-images": {
            "action": "store_true",
            "default": False,
            "help": "Set to True to disable screenshot capturing, False by default",
        },
    }

    def __init__(self, env, mach_cmd):
        super(WebPageTest, self).__init__(env, mach_cmd)
        if utils.ON_TRY:
            self.WPT_key = utils.get_tc_secret(wpt=True)["wpt_key"]
        else:
            self.WPT_key = pathlib.Path(HERE, "WPT_key.txt").open().read()
        self.statistic_types = ["average", "median", "standardDeviation"]
        self.timeout_limit = 21600
        self.wait_between_requests = 5

    def run(self, metadata):
        options = metadata.script["options"]
        test_list = options["test_list"]
        self.statistic_types = options["test_parameters"]["statistics"]
        self.pre_run_error_checks(options["test_parameters"], test_list)
        self.create_and_run_wpt_threaded_tests(test_list, metadata)
        return metadata

    def pre_run_error_checks(self, options, test_list):
        if options["browser"] not in ACCEPTED_BROWSERS:
            raise WPTBrowserSelectionError(
                "Invalid Browser Option Selected, please choose one of the following: "
                f"{ACCEPTED_BROWSERS}"
            )
        if options["connection"] not in ACCEPTED_CONNECTIONS:
            raise WPTInvalidConnectionSelection(
                "Invalid Connection Option Selected, please choose one of the following: "
                f"{ACCEPTED_CONNECTIONS}"
            )
        if not len(self.statistic_types):
            raise WPTInvalidStatisticsError(
                "No statistics provided please provide some"
            )
        for stat in self.statistic_types:
            if stat not in ACCEPTED_STATISTICS:
                raise WPTInvalidStatisticsError(
                    f"This is an invalid statistic, statistics can only be from "
                    f"the following list: {ACCEPTED_STATISTICS}"
                )

        if "timeout_limit" in options.keys():
            self.timeout_limit = options["timeout_limit"]
        if "wait_between_requests" in options.keys():
            self.wait_between_requests = options["wait_between_requests"]
        if "statistics" in options.keys():
            self.statistic_types = options["statistics"]

        options["capture_video"] = 0 if self.get_arg("no-video") else options["video"]
        options["noimages"] = 1 if self.get_arg("no-images") else options["noimages"]
        self.location_queue(options["location"])
        self.check_urls_are_valid(test_list)

    def location_queue(self, location):
        location_list = self.request_with_timeout(
            "https://www.webpagetest.org/getLocations.php?f=json"
        )["data"]
        if location not in location_list.keys():
            raise WPTLocationSelectionError(
                "Invalid location selected please choose one of the locations here: "
                f"{location_list.keys()}"
            )
        self.info(
            f"Test queue at {location}({location_list[location]['Label']}) is "
            f"{location_list[location]['PendingTests']['Queued']}"
        )

    def request_with_timeout(self, url):
        results_of_test = {}
        start = time.time()
        while (
            results_of_test.get("statusCode") != 200
            and time.time() - start < self.timeout_limit
        ):
            results_of_test = json.loads(requests.get(url).text)
            time.sleep(self.wait_between_requests)
        if results_of_test.get("statusCode") != 200:
            raise WPTTimeOutError(
                f"{url} test timed out after {self.timeout_limit} seconds"
            )
        return results_of_test

    def check_urls_are_valid(self, test_list):
        for url in test_list:
            if "." not in url:
                raise WPTInvalidURLError(f"{url} is an invalid url")

    def create_and_run_wpt_threaded_tests(self, test_list, metadata):
        threads = []
        for website in test_list:
            t = PropagatingErrorThread(
                target=self.create_and_run_wpt_tests, args=(website, metadata)
            )
            t.start()
            threads.append(t)
        for thread in threads:
            thread.join()

    def create_and_run_wpt_tests(self, website_to_be_tested, metadata):
        wpt_run = self.get_WPT_results(
            website_to_be_tested, metadata.script["options"]["test_parameters"]
        )
        self.post_run_error_checks(
            wpt_run, metadata.script["options"], website_to_be_tested
        )
        self.add_wpt_run_to_metadata(wpt_run, metadata, website_to_be_tested)

    def get_WPT_results(self, website, options):
        self.info(f"Testing: {website}")
        wpt_test_request_link = self.create_wpt_request_link(options, website)
        send_wpt_test_request = self.request_with_timeout(wpt_test_request_link)[
            "data"
        ]["jsonUrl"]
        results_of_test = self.request_with_timeout(send_wpt_test_request)
        return results_of_test

    def create_wpt_request_link(self, options, website_to_be_tested):
        test_parameters = ""
        for key_value_pair in list(options.items())[6:]:
            test_parameters += "&{}={}".format(*key_value_pair)
        return (
            f"https://www.webpagetest.org/runtest.php?url={website_to_be_tested}&k={self.WPT_key}&"
            f"location={options['location']}:{options['browser']}.{options['connection']}&"
            f"f=json{test_parameters}"
        )

    def post_run_error_checks(self, results_of_test, options, url):
        self.info(f"{url} test can be found here: {results_of_test['data']['summary']}")

        if results_of_test["data"]["testRuns"] != results_of_test["data"][
            "successfulFVRuns"
        ] or (
            not results_of_test["data"]["fvonly"]
            and results_of_test["data"]["testRuns"]
            != results_of_test["data"]["successfulRVRuns"]
        ):
            """
            This error is raised in 2 conditions:
            1) If the testRuns requested does not equal the successfulFVRuns(Firstview runs)
            2) If repeat view is enabled and if testRuns requested does not equal successfulFVRuns
             and successfulRVRuns
            """
            # TODO: establish a threshold for failures, maybe fail after data processing
            raise WPTErrorWithWebsite(
                f"Something went wrong with firstview/repeat view runs for: {url}"
            )
        self.confirm_correct_browser_and_location(
            results_of_test["data"], options["test_parameters"]
        )

    def confirm_correct_browser_and_location(self, data, options):
        if data["location"] != f"{options['location']}:{options['browser']}":
            raise WPTBrowserSelectionError(
                "Resulting browser & location are not aligned with submitted browser & location"
            )

    def add_wpt_run_to_metadata(self, wbt_run, metadata, website):
        requested_values = self.extract_desired_values_from_wpt_run(wbt_run)
        metadata.add_result(
            {
                "name": ("WebPageTest:" + re.match(r"(^.\w+)", website)[0]),
                "framework": {"name": "mozperftest:"},
                "transformer": "mozperftest.test.webpagetest:WebPageTestData",
                "results": [
                    {
                        "values": [int(metric_value)],
                        "name": metric_name,
                    }
                    for metric_name, metric_value in requested_values.items()
                ],
            }
        )

    def extract_desired_values_from_wpt_run(self, wpt_run):
        view_types = ["firstView"]
        if not wpt_run["data"]["fvonly"]:
            view_types.append("repeatView")
        desired_values = {}
        for statistic in self.statistic_types:
            for view in view_types:
                for value in WPT_METRICS:
                    if value not in wpt_run["data"][statistic][view].keys():
                        raise WPTDataProcessingError(f"{value} not found wpt results ")
                    desired_values[f"{value}.{view}.{statistic}"] = wpt_run["data"][
                        statistic
                    ][view][value]
        return desired_values
