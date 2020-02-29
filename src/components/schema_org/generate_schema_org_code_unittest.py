# Copyright 2020 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Tests for generate_schema_org_code."""

import sys
import unittest
import generate_schema_org_code
from generate_schema_org_code import schema_org_id
import os

SRC = os.path.join(os.path.dirname(__file__), os.path.pardir, os.path.pardir)
sys.path.append(os.path.join(SRC, 'third_party', 'pymock'))
import mock

_current_dir = os.path.dirname(os.path.realpath(__file__))
# jinja2 is in chromium's third_party directory
# Insert at front to override system libraries, and after path[0] == script dir
sys.path.insert(
    1, os.path.join(_current_dir, *([os.pardir] * 2 + ['third_party'])))
import jinja2


class GenerateSchemaOrgCodeTest(unittest.TestCase):
    def test_get_template_vars(self):
        file_content = """
    {
      "@graph": [
        {
          "@id": "http://schema.org/MediaObject",
          "@type": "rdfs:Class"
        },
        {
          "@id": "http://schema.org/propertyName",
          "@type": "rdf:Property"
        }
      ]
    }
    """
        with mock.patch('__builtin__.open',
                        mock.mock_open(read_data=file_content)) as m_open:
            self.assertEqual(
                generate_schema_org_code.get_template_vars(m_open), {
                    'entities': ['MediaObject'],
                    'properties': [{
                        'name': 'propertyName',
                        'thing_types': []
                    }]
                })

    def test_get_root_type_thing(self):
        thing = {'@id': schema_org_id('Thing')}
        intangible = {
            '@id': schema_org_id('Intangible'),
            'rdfs:subClassOf': thing
        }
        structured_value = {
            '@id': schema_org_id('StructuredValue'),
            'rdfs:subClassOf': intangible
        }
        schema = {'@graph': [thing, intangible, structured_value]}

        self.assertEqual(
            generate_schema_org_code.get_root_type(structured_value, schema),
            thing)

    def test_get_root_type_datatype(self):
        text = {
            '@id': schema_org_id('Text'),
            '@type': [schema_org_id('DataType'), 'rdfs:Class']
        }
        url = {'@id': schema_org_id('URL'), 'rdfs:subClassOf': text}
        schema = {'@graph': [url, text]}

        self.assertEqual(
            generate_schema_org_code.get_root_type(url, schema), text)

    def test_parse_property_identifier(self):
        thing = {'@id': schema_org_id('Thing')}
        intangible = {
            '@id': schema_org_id('Intangible'),
            'rdfs:subClassOf': thing
        }
        structured_value = {
            '@id': schema_org_id('StructuredValue'),
            'rdfs:subClassOf': intangible
        }
        property_value = {
            '@id': schema_org_id('PropertyValue'),
            'rdfs:subClassOf': structured_value
        }
        text = {
            '@id': schema_org_id('Text'),
            '@type': [schema_org_id('DataType'), 'rdfs:Class']
        }
        url = {'@id': schema_org_id('URL'), 'rdfs:subClassOf': text}
        identifier = {
            '@id': schema_org_id('Identifier'),
            schema_org_id('rangeIncludes'): [property_value, url, text]
        }
        schema = {
            '@graph': [
                thing, intangible, structured_value, property_value, text, url,
                identifier
            ]
        }

        self.assertEqual(
            generate_schema_org_code.parse_property(identifier, schema), {
                'name': 'Identifier',
                'has_text': True,
                'thing_types': [property_value['@id']]
            })


if __name__ == '__main__':
    unittest.main()
