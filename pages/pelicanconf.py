#!/usr/bin/env python
# -*- coding: utf-8 -*- #

AUTHOR = 'Paul-Louis Ageneau'
COPYRIGHT_YEAR = 2021
SITENAME = 'libdatachannel'
SITEURL = ''

PATH = 'content'
STATIC_PATHS = ['images', 'extra/CNAME']
EXTRA_PATH_METADATA = {'extra/CNAME': {'path': 'CNAME'}}

THEME = 'theme'

TIMEZONE = 'Europe/Paris'
DEFAULT_LANG = 'en'

GITHUB_URL = "https://github.com/paullouisageneau/libdatachannel"

# Feed generation is usually not desired when developing
FEED_ALL_ATOM = None
CATEGORY_FEED_ATOM = None
TRANSLATION_FEED_ATOM = None
AUTHOR_FEED_ATOM = None
AUTHOR_FEED_RSS = None

# Blogroll
#
#LINKS = (('Pelican', 'https://getpelican.com/'),
#         ('Python.org', 'https://www.python.org/'),
#         ('Jinja2', 'https://palletsprojects.com/p/jinja/'),
#         ('You can modify those links in your config file', '#'),)

# Social widget
#SOCIAL = (('You can add links in your config file', '#'),
#          ('Another social link', '#'),)

DEFAULT_PAGINATION = False

# Uncomment following line if you want document-relative URLs when developing
#RELATIVE_URLS = True

MARKDOWN = {
    'tab_length': 2,
}